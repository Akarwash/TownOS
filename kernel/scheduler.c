#include "scheduler.h"
#include "syscall.h"     // SYSCALL_ERROR: the value SYS_WAIT returns to a childless caller
#include "gdt.h"
#include "usermode.h"
#include "heap.h"
#include "paging.h"
#include "memory.h"
#include "elf.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"   // the LIFECYCLE_DEBUG reap report below

// Pointers to the heap-allocated tasks, in creation order (ids 0, 1, ...). Each
// task_t is kmalloc'd in task_register. A flat pointer array (rather than a linked
// list) keeps schedule()'s O(1) round-robin indexing a mechanical change from the
// old fixed array.
//
// SLOTS 0..num_tasks-1 MAY CONTAIN NULL. This used to be a dense array where a
// non-NULL pointer was guaranteed, and that assumption is now wrong: a reaped task
// has its struct kfree'd and its slot set back to NULL, leaving a HOLE. The id is
// never reused (a hole stays a hole forever, see num_tasks below), so an id remains
// a permanent, unambiguous name for one task and a stale id can never silently
// address a different one. The cost is that EVERY loop over this array must skip
// NULL: a missing skip dereferences a freed pointer and page-faults at a low
// address, tens of seconds after the task that made the hole exited, which reads
// like a fault with no cause. All of them are marked below.
//
// A task_t is kernel-only bookkeeping (only the code here reads it), never touched
// by ring-3 code, so it is safe on kernel (non-PG_USER) heap pages. That is why
// the STRUCT can go on the heap but the STACK below cannot.
static task_t *tasks[MAX_TASKS_LIMIT];

// Flags for a ring-3 stack page: present, writable, reachable at CPL 3. Program
// pages are mapped by the ELF loader instead, which derives their flags from the
// segment's own (kernel/elf.c), so a text segment can be mapped read-only.
#define USER_PAGE_FLAGS  (PG_PRESENT | PG_WRITABLE | PG_USER)

// Map a fresh stack into `as` at the fixed stack VA (USER_STACK_BASE up to
// USER_STACK_TOP, both in usermode.h). Fresh frames, no copy: the program writes
// its own stack as it runs. Every task's stack is at the SAME virtual address on
// its OWN physical frames, which is what per-process paging buys and what
// replaced the old bump allocator that split PD[3] by hand.
//
// Returns 0 on success, -1 if a frame or page-table allocation fails. On failure
// the frames already mapped into `as` leak, which is acceptable here: it only
// happens on out-of-memory, and tasks are never destroyed.
static int map_user_stack(address_space_t *as) {
    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += FRAME_SIZE) {
        uint64_t frame = alloc_frame();
        if (frame == 0) {
            return -1;
        }
        if (paging_map_page(as, va, frame, USER_PAGE_FLAGS) != 0) {
            return -1;
        }
    }
    return 0;
}

// Index of the task currently on the CPU.
static uint32_t current = 0;

// A HIGH WATER MARK, not a count of live tasks. Tasks are handed ids 0, 1, ... in
// creation order and this only ever grows: reaping a task frees its struct and
// NULLs its slot, but does NOT decrement this, because doing so would let the next
// creation hand out an id that is already the name of a dead task. A parent holding
// the id of a child it has not waited for would then be pointed at a stranger. So
// the walks below are bounded by this and skip the holes, and the array fills up
// over the life of the machine even though the live task count may not grow at all.
static uint32_t num_tasks = 0;

// Set while schedule() is parked in its all-blocked idle loop (idle_until_runnable
// below). The idle loop runs with interrupts ENABLED, which is the whole point, so
// timer ticks keep arriving and keep calling schedule() while we sit there. Those
// nested calls must do nothing and return: if a nested call parked in the idle loop
// too, every tick would nest one level deeper on the single shared kernel stack and
// a long idle would overflow it. With this flag the nesting depth stays at one, the
// nested tick unwinds back into the hlt loop, and the OUTER call (which owns the
// live pile we are going to overwrite) is the one that performs the switch.
static int scheduler_idling = 0;

// Guard against a startup race. The timer starts ticking the instant isr_install
// runs sti (long before scheduler_start), and each tick calls schedule(). Until
// task 0 has actually been entered, those early ticks fire in kernel (CPL 0)
// context and there is nothing to switch: schedule() must NOT save that kernel
// pile over a forged task or copy a forged task onto the kernel stack. This flag
// stays 0 until scheduler_start arms it, so schedule() is a no-op before then.
static int scheduler_running = 0;

// Register a task whose address space is already built, and forge its saved pile
// so iretq will "return" into a program that never ran.
//
// Both creation paths end here, so the forge exists in exactly one place. The
// only thing that differs between a compiled-in program and one loaded from a
// file is where `entry` came from: a linker symbol, or an ELF header.
//
// `parent_id` is stamped on the new task and never changes. It is recorded at
// creation because it cannot be recovered afterwards, and two things read it: the
// parent's SYS_WAIT, to find its children, and the reap sweeper, to decide whether
// a zombie still has anybody who might read its exit status.
//
// Returns the task id, or -1 if the heap is full or the task table is.
static int task_register(address_space_t *as, uint64_t entry, uint32_t parent_id) {
    if (num_tasks >= MAX_TASKS_LIMIT) {
        return -1;                          // bookkeeping array full (arbitrary cap)
    }

    // The task_t is kernel-only bookkeeping, so it is safe on the kernel heap.
    // This is what removes the old fixed-4 ceiling on task structs.
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL) {
        return -1;                          // out of heap: same contract as old "table full"
    }

    uint32_t id = num_tasks++;
    tasks[id] = t;

    t->aspace = as;
    t->cr3 = as->pml4_phys;                 // cached so schedule() need not deref

    // Forge the pile so iretq will "return" into a program that never ran. This
    // is the exact trick enter_user_mode uses (usermode.c), generalised: instead
    // of pushing the five iretq values by hand and running iretq now, we fill the
    // registers_t the scheduler will later copy onto the stack, and let the
    // timer's iretq consume it.
    memset(&t->regs, 0, sizeof(t->regs));   // all GPRs start at 0
    t->regs.rip = entry;                    // first instruction (0x400000 region)
    t->regs.user_rsp = USER_STACK_TOP;      // fixed stack top, same VA in every task
    t->regs.cs = GDT_SELECTOR_USER_CODE;    // 0x1B: ring-3 code, RPL 3
    t->regs.ss = GDT_SELECTOR_USER_DATA;    // 0x23: ring-3 data, RPL 3

    // rflags bit 9 (IF) MUST be set. If IF is clear the task runs with interrupts
    // masked, the timer never fires, and this task is never preempted: it owns
    // the machine forever and no other task ever runs. USER_MODE_RFLAGS (0x202)
    // has bit 1 (reserved, always 1) and bit 9 (IF) set.
    t->regs.rflags = USER_MODE_RFLAGS;

    t->id = id;
    t->state = TASK_READY;
    t->wait_reason = WAIT_NONE;   // only meaningful once the task blocks
    t->parent_id = parent_id;     // who to wake and who may read the status below
    t->exit_status = 0;           // only meaningful once the task is a TASK_ZOMBIE
    return (int)id;
}

int task_create_from_file(const char *name, uint32_t parent_id) {
    // Same shape as task_create, and deliberately so: private address space,
    // user half filled, stack mapped, frame forged. The ONLY differences are
    // where the program's bytes come from (a file on the disk rather than a
    // range of the kernel image) and where the entry address comes from (the
    // ELF header rather than a linker symbol). Nothing about the page tree, the
    // stack, the forge, the scheduler or the CR3 switch changes.
    address_space_t *as = paging_create_address_space();
    if (as == NULL) {
        return -1;
    }

    uint64_t entry = 0;
    if (elf_load_file(name, as, &entry) != 0) {
        return -1;      // elf_load_file has already said what was wrong with it
    }

    if (map_user_stack(as) != 0) {
        return -1;
    }

    return task_register(as, entry, parent_id);
}

uint32_t scheduler_current_id(void) {
    // `current` is file-static on purpose: nothing outside this file may index the
    // task table. A syscall handler still needs to know WHO is asking (SYS_RUN
    // stamps the new task's parent_id with it), so the id is exported and the table
    // is not.
    return current;
}

void scheduler_start(void) {
    // Close the last sliver of the startup race. Between arming scheduler_running
    // and the iretq inside enter_user_mode there are a handful of instructions; a
    // timer tick landing there would try to switch while we are mid-handoff. cli
    // masks interrupts for those few instructions. It is safe because the first
    // thing that happens on entering task 0 is iretq restoring rflags = 0x202,
    // which sets IF again, so the timer resumes the moment ring-3 code runs.
    __asm__ __volatile__("cli");

    current = 0;
    tasks[0]->state = TASK_RUNNING;
    scheduler_running = 1;

    // Load task 0's address space BEFORE dropping to ring 3. Until now we have run
    // on the boot tables; task 0's user code and stack live in ITS tree, not the
    // boot tree, so entering ring 3 without this switch would fetch the first user
    // instruction through a mapping that no longer describes this task. This is
    // safe because the kernel half is cloned identically into task 0's tree, so
    // enter_user_mode (kernel code) and the task_t it reads (kernel heap) stay
    // mapped across the switch; only the user half changes. Every LATER switch is
    // done by schedule(); this is just the first one, which schedule() never runs.
    paging_switch(tasks[0]->aspace);

    // Reuse the proven ring-3 entry path rather than hand-rolling a second iretq.
    // Task 0's forged GPRs (all zero) do not matter on this first entry: a fresh
    // program sets up its own registers before it reads any. Every LATER entry
    // into task 0 goes through schedule(), which restores its full saved pile.
    enter_user_mode(tasks[0]->regs.rip, tasks[0]->regs.user_rsp);
}

// Is any task runnable at all? Used only by the idle loop, which asks about the
// whole table rather than about a position in the rotation.
static int any_task_ready(void) {
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (tasks[i] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[i]->state == TASK_READY) {
            return 1;
        }
    }
    return 0;
}

// The round-robin pick. Walk forward from the task after `from`, wrapping, and
// return the first TASK_READY slot, or -1 if there is none.
//
// Testing for TASK_READY (rather than "not UNUSED") is what makes a blocked task
// invisible to the rotation: it is stepped straight over, however many times the
// cursor comes round, until whatever it waits for marks it READY again. Candidate
// num_tasks is `from` itself, so a task that is still runnable keeps the CPU when
// nothing else wants it, and a task that has just BLOCKED itself does not match
// and so cannot be handed back the CPU it just gave up. The same test also makes a
// TASK_ZOMBIE unreachable: a dead task is never READY, so the rotation can never
// hand the CPU back to one, and no separate check for it is needed here.
//
// The walk is bounded by num_tasks, which is a HIGH WATER MARK and no longer the
// count of live tasks: slots 0..num_tasks-1 used to be exactly the live tasks, and
// since reaping began that is false. The cursor therefore steps over NULL holes as
// well as blocked tasks, and the array is scanned in full even when few tasks live.
static int find_next_ready(uint32_t from) {
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t cand = (from + i) % num_tasks;
        if (tasks[cand] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[cand]->state == TASK_READY) {
            return (int)cand;
        }
    }
    return -1;
}

// Everyone is blocked. Park the CPU until an interrupt makes someone runnable.
//
// INTERRUPTS MUST BE ENABLED HERE. This is the one place in the kernel where that
// is not just preferable but load-bearing: the ONLY thing that can produce a READY
// task is an interrupt handler (today the keyboard IRQ waking a WAIT_KEY task), so
// halting with interrupts masked would mean nothing could ever wake anyone and the
// machine would be dead, not idle. That is the deadlock to avoid.
//
// `hlt` with interrupts enabled is what makes "everyone asleep" cost ZERO CPU
// rather than spin. The CPU stops executing entirely and draws no power until a
// hardware interrupt arrives, instead of whirling through a loop that re-reads a
// variable nothing in this thread can change. Spinning here would be the same
// mistake as the busy-wait this whole change exists to remove, just moved into the
// scheduler.
//
// `sti; hlt` in one breath is deliberate and must stay adjacent. sti takes effect
// only AFTER the following instruction, precisely so this pair is atomic: an
// interrupt cannot slip into the gap, find nothing to wake, and leave us halted
// forever with the wakeup already spent. The condition is re-read with interrupts
// off (we enter with IF clear from the interrupt gate, and cli again after each
// wake), so a wakeup cannot be missed between the test and the halt.
static void idle_until_runnable(void) {
    scheduler_idling = 1;
    while (!any_task_ready()) {
        __asm__ __volatile__("sti; hlt; cli");
    }
    scheduler_idling = 0;
}

// Length in bytes of the `int 0x50` instruction that brings a task into a syscall.
// The opcode is CD ib: one byte of opcode, one immediate byte carrying the vector.
// Named, because a bare 2 buried in pointer arithmetic on a saved rip is unreadable
// and unsearchable.
#define INT_INSTR_LEN 2

void task_block(registers_t *r, wait_reason_t reason) {
    // (1) Take this task out of the rotation and record what it is waiting for, so
    // the waker for that event can find it again (see scheduler_wake).
    tasks[current]->state = TASK_BLOCKED;
    tasks[current]->wait_reason = reason;

    // (2) RE-ARM THE SYSCALL: resume ON the int, not after it, so the woken task
    // re-issues the syscall.
    //
    // This is the heart of the design and the part worth understanding. We cannot
    // freeze this task where it stands, half way through a C function in the
    // kernel, and thaw it here later. Two things in this kernel forbid it. The
    // saved pile holds the rip the CPU pushed on the ring-3 to ring-0 transition,
    // so it is always a RING-3 address, never a kernel one: restoring a pile can
    // only ever resume user code. And there is a single kernel stack shared by
    // every task (tss.rsp0, see gdt.c), so the C frames we are standing on right
    // now are abandoned the instant we switch away, and the next task to enter the
    // kernel writes over them.
    //
    // So instead of resuming in the middle, we resume at the beginning. The CPU
    // pushed the address of the instruction AFTER `int 0x50`; winding it back by
    // the length of that instruction points it at the int itself. When this task is
    // eventually woken and rescheduled, its iretq lands on the int, the syscall is
    // issued again from scratch, and this time it finds what it was waiting for
    // (that is precisely what being woken means) and returns normally.
    //
    // This only makes sense because the caller entered through `int 0x50`. A task
    // that reached here any other way would have its rip wound back into the middle
    // of whatever instruction happens to precede it, which is garbage. That
    // invariant is enforced by convention, not by the type system: see the header.
    r->rip -= INT_INSTR_LEN;

    // (3) Switch away through the SAME routine the timer uses. schedule() saves the
    // (now rewound) pile into this task, picks a READY task, overwrites the live
    // pile and loads the new CR3, so the iretq at the end of syscall_common_stub
    // returns into a different task. The only difference from the timer path is
    // what prompted the call, which is why one routine serves both: involuntary
    // preemption and a voluntary block are the same switch.
    //
    // No EOI concern here, unlike the timer path: `int 0x50` is a software
    // interrupt, so the PIC has no in-service bit to acknowledge.
    //
    // Interrupt-flag discipline matches the timer path exactly. Both handlers are
    // reached through interrupt gates, so IF is clear throughout, and the iretq
    // restores the incoming task's own rflags (IF set) as it returns to ring 3.
    // Nothing here leaves interrupts disabled across the yield, which matters: the
    // keyboard IRQ that will wake this task has to be able to fire.
    schedule(r);

    // Unreachable on the blocking path: schedule() redirected the pile, so this
    // kernel entry now belongs to another task and ends at that iretq.
}

void scheduler_wake(wait_reason_t reason) {
    // The block and the wake are a matched pair, and the pairing rule is that
    // whoever CAUSES an event wakes the tasks waiting on it. A blocked task cannot
    // wake itself: it is not running, so it cannot notice anything. That is the
    // whole point, and it is why this lives here and is called from the driver that
    // produced the event rather than from anything the sleeper does.
    //
    // A linear scan over every task, which is fine at this scale (a handful of
    // tasks) and is the honest simple thing. A kernel with many blocked tasks would
    // keep a per-reason wait queue instead and wake off the head in constant time.
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (tasks[i] == NULL) {
            continue;               // a reaped task's hole (see tasks[] above)
        }
        if (tasks[i]->state == TASK_BLOCKED && tasks[i]->wait_reason == reason) {
            tasks[i]->state = TASK_READY;
            tasks[i]->wait_reason = WAIT_NONE;
        }
    }

    // Deliberately no context switch here. This runs in interrupt context, where
    // the live pile belongs to whatever was interrupted, not to the task we just
    // woke, so switching would be both wrong and unnecessary: the woken task is
    // back in the rotation and the next tick's schedule() will reach it.
}

// Report every teardown on the console, with the number of free frames after it.
//
// ON BY DEFAULT, AND DELIBERATELY SO. This one line is the leak test for the whole
// lifecycle: run the same program ten times and the count printed after each must
// be identical from the second onwards. A slow monotonic decrease means something a
// dead task held is not coming back (most likely a page table under one of the two
// user PD slots, or the address_space_t handle), and without this the only symptom
// would be a machine that runs out of memory after a long session, with nothing to
// point at the cause. Set to 0 to silence it.
#define LIFECYCLE_DEBUG 1

// Called at the moment a dead task's address space has just gone back to the pools,
// from whichever of the two teardown paths got there first (see task_wait for why
// there are two). It is a function rather than a line in each path so the two
// cannot drift into reporting different things.
static void lifecycle_report_reap(uint32_t id, int32_t status) {
#if LIFECYCLE_DEBUG
    print_string("reap: task ");
    print_int(id);
    print_string(" exited (status ");
    print_int((uint32_t)status);
    print_string("), free frames: ");
    print_int((uint32_t)frame_free_count());
    print_string("\n");
#else
    (void)id;
    (void)status;
#endif
}

// Is there still somebody who could read a zombie's exit status?
//
// This is not simply "does the slot exist". A ZOMBIE PARENT DOES NOT COUNT. It has
// exited itself, so it will never issue another SYS_WAIT, and treating it as a live
// reader would keep its dead children's tombstones alive forever waiting on a call
// that cannot come: a permanent leak of one task_t per orphan, invisible in a free
// frame count because a task_t is a heap block rather than a frame.
static int parent_alive(uint32_t parent_id) {
    if (parent_id == TASK_NO_PARENT) {
        return 0;              // kernel_main started this task; nobody can wait on it
    }
    if (parent_id >= num_tasks) {
        return 0;              // an id never handed out (defensive: cannot happen)
    }
    if (tasks[parent_id] == NULL) {
        return 0;              // the parent has itself been reaped
    }
    return tasks[parent_id]->state != TASK_ZOMBIE;
}

// Free what the dead are still holding. Called from schedule() only, once per tick.
//
// The split of who frees what is the heart of this design. THE HEAVY RESOURCES (the
// user frames, the page tables, the address_space_t) ARE FREED HERE; the light ones
// (the task_t and its slot) are freed by the parent at wait time, because they hold
// the exit status the parent has not read yet. Splitting them is what lets the
// memory come back promptly without losing the answer to "how did it end".
static void reap_sweep(void) {
    for (uint32_t i = 0; i < num_tasks; i++) {
        task_t *t = tasks[i];
        if (t == NULL || t->state != TASK_ZOMBIE) {
            continue;
        }

        // THE SAFETY ARGUMENT OF THE ENTIRE RUNG IS THIS ONE LINE. On the tick where
        // a task exits, `current` is STILL that task and ITS page tables are STILL
        // loaded in CR3: task_exit does paperwork and calls schedule(), and the CR3
        // write that moves off this tree happens at the very end of schedule(),
        // below. Destroying the tree here would hand the frames the CPU is at this
        // moment translating through back to the free pool. Nothing would fault now;
        // the frames would be reissued to the next task's page tables and the
        // machine would die later, somewhere unrelated. Skipping `current` means a
        // task is only ever swept on a LATER tick, by which time CR3 has moved on.
        if (i == current) {
            continue;
        }

        if (t->aspace != NULL) {
            paging_destroy_address_space(t->aspace);
            t->aspace = NULL;   // the tree is gone: never load or free it twice
            t->cr3 = 0;         // and never let this stale value reach CR3
            lifecycle_report_reap(t->id, t->exit_status);
        }

        // An orphan loses its tombstone. With nobody left to call SYS_WAIT, the
        // exit status has no reader, so keeping the struct would keep a fact nobody
        // will ever ask for. The id is NOT recycled: the slot stays NULL forever
        // (see num_tasks above), so nothing can be confused about who this was.
        if (!parent_alive(t->parent_id)) {
            kfree(t);
            tasks[i] = NULL;
        }
    }
}

void task_exit(registers_t *r, int status) {
    task_t *t = tasks[current];

    // (1) Record how it ended, MASKED TO 0..255. The mask is not cosmetic: SYS_WAIT
    // returns the status in RAX and returns SYSCALL_ERROR (-1) when the caller has
    // no children at all, so an unmasked status of -1 would be indistinguishable
    // from "you have no children" and a parent would mis-report a child that exited
    // perfectly normally. Masking makes the two value ranges disjoint by
    // construction rather than by hoping no program picks the wrong number.
    t->exit_status = status & 0xFF;

    // (2) Off the rotation for good. TASK_ZOMBIE is not TASK_BLOCKED: nothing will
    // ever wake it, and find_next_ready only ever returns TASK_READY slots, so the
    // CPU can never be handed back to it. wait_reason is cleared because it is only
    // meaningful while BLOCKED, and a zombie carrying a stale reason would be woken
    // by the next scheduler_wake for that reason and marked READY again.
    t->state = TASK_ZOMBIE;
    t->wait_reason = WAIT_NONE;

    // (3) Wake the parent if it is asleep in SYS_WAIT, BY ID, not by reason.
    //
    // scheduler_wake(WAIT_CHILD) would be wrong here, and would look right. It
    // readies EVERY task blocked on WAIT_CHILD, town-wide; the other waiting parents'
    // children have not exited, so each would be scheduled, re-issue its wait, find
    // no zombie among its own children, and block again. Worse, the wake is what
    // publishes "your child is ready to be reaped", so broadcasting it announces
    // something untrue to everyone but one task. WAIT_CHILD is the first wait reason
    // whose event belongs to ONE specific sleeper rather than to whoever is
    // listening, which is why it is the first one woken by id.
    //
    // THIS MUST HAPPEN BEFORE schedule(). If the parent is still BLOCKED when
    // schedule() runs and it is the only other task in the town, find_next_ready
    // returns -1, schedule() parks in idle_until_runnable, and nothing can ever make
    // anyone ready again: the only task that could have woken the parent is this one,
    // and it is already dead. The machine sits in `hlt` forever with no message. That
    // is a deadlock built out of two individually correct pieces, so the ordering is
    // load-bearing and not merely tidy.
    if (parent_alive(t->parent_id)) {
        task_t *p = tasks[t->parent_id];
        if (p->state == TASK_BLOCKED && p->wait_reason == WAIT_CHILD) {
            p->state = TASK_READY;
            p->wait_reason = WAIT_NONE;
        }
    }

    // (4) Switch away, through the same routine the timer drives.
    //
    // NOTE WHAT IS NOT HERE. There is no rip rewind: that is task_block's trick, for
    // a task that will run again and re-issue its syscall, and this task must never
    // run again. Nothing is freed: this task's page tables are still in CR3 right
    // now, so the frames go back on a later tick, from the sweeper above, which is
    // the only context that is guaranteed not to be standing on them. And rax is not
    // written, because there is nobody left to return a value to.
    schedule(r);

    // Unreachable: schedule() redirected the pile into another task, and no path
    // will ever pick a TASK_ZOMBIE again.
}

void task_wait(registers_t *r) {
    int have_child = 0;

    for (uint32_t i = 0; i < num_tasks; i++) {
        task_t *t = tasks[i];
        if (t == NULL || t->parent_id != current) {
            continue;
        }
        have_child = 1;

        if (t->state != TASK_ZOMBIE) {
            continue;           // alive: keep looking, it may not be the only child
        }

        // A child has already exited. Take its status, then take it apart.
        int32_t status = t->exit_status;

        // Freeing the address space HERE as well as in the sweeper is DELIBERATE and
        // is not a duplicated responsibility to be tidied away. A parent can call
        // SYS_WAIT on the very tick its child exited, before the sweeper has had a
        // chance to run, and this path is about to free the task_t: without this the
        // struct (and with it the only pointer to the tree) would be gone and the
        // whole address space would leak. Both paths guard on aspace != NULL, so
        // whichever arrives first does the work and the other does nothing.
        //
        // In practice THIS path usually wins, which is worth knowing before reading
        // the sweeper as the normal case. task_exit wakes the parent and switches
        // straight to it, the parent's iretq lands on its rewound `int 0x50`, and it
        // re-enters this function without a timer tick in between, so the sweeper
        // has had no opportunity to run. The sweeper's copy is what collects a child
        // whose parent is slow to wait, or never waits at all.
        //
        // It is safe here for the same reason the sweeper's skip of `current` makes
        // it safe there: this is a CHILD of the caller, and a child is by definition
        // not the task running right now, so this tree is not the one in CR3.
        if (t->aspace != NULL) {
            paging_destroy_address_space(t->aspace);
            t->aspace = NULL;
            t->cr3 = 0;
            lifecycle_report_reap(t->id, status);
        }

        kfree(t);
        tasks[i] = NULL;        // a permanent hole; the id is never reused

        r->rax = (uint64_t)(int64_t)status;
        return;
    }

    if (!have_child) {
        // Nothing to wait for, and nothing that could ever arrive. Blocking here
        // would be an unbreakable sleep: only task_exit wakes a WAIT_CHILD sleeper,
        // and with no children nothing can ever call it on this task's behalf. Fail
        // loudly-in-RAX instead. SYSCALL_ERROR is safe to write because this path
        // does NOT block (see below).
        r->rax = SYSCALL_ERROR;
        return;
    }

    // Children exist but none has exited yet. Sleep until one does.
    //
    // NOTHING MAY WRITE r->rax ON THIS PATH, and this is the second syscall to carry
    // that rule (the first is sys_readkey, kernel/syscall.c, where it is documented
    // at length). task_block rewinds rip onto the `int 0x50`, so when the woken task
    // re-executes that instruction the CPU reads the SYSCALL NUMBER out of RAX.
    // Leaving a return value there makes the woken task issue a DIFFERENT syscall,
    // and here the accident is spectacular rather than subtle: SYS_EXIT is 0, so
    // writing 0 (a perfectly ordinary "child exited successfully" status) would make
    // the woken parent kill itself instead of reporting the result. The shell is
    // usually that parent.
    task_block(r, WAIT_CHILD);
}

void schedule(registers_t *r) {
    // Ignore ticks that fire before task 0 has been entered (see the flag above).
    if (!scheduler_running) {
        return;
    }

    // A tick that landed while the outer call is parked in idle_until_runnable has
    // nothing useful to do and must not park as well (see scheduler_idling above).
    // Returning here unwinds it straight back into that hlt loop, which re-checks.
    if (scheduler_idling) {
        return;
    }

    // Collect what the dead are still holding, before anything else happens.
    //
    // THE PLACEMENT IS THE DESIGN, not a convenience. It has to be INSIDE schedule()
    // because schedule() is the only code in the kernel that runs after a CR3 switch
    // has moved off a dead task's tree; anywhere else (in task_exit, in the syscall
    // dispatcher, in a timer callback) would still be standing on the address space
    // it is trying to free. And it has to skip `current`, which it does, because on
    // the tick where a task exits, `current` is still that very task.
    //
    // It runs after the two guards above so it never fires before the scheduler is
    // armed or while a nested tick is unwinding out of the idle loop, and before the
    // save below so a freed task's memory is out of the way before this tick starts
    // shuffling piles around.
    reap_sweep();

    // DEFENSIVE, AND IT SHOULD BE UNREACHABLE. Every other loop over tasks[] skips
    // NULL because a reaped task leaves a hole, but the CURRENT slot is different:
    // it should never be a hole by construction. Only two things free a task_t, and
    // neither can free this one. The sweeper below explicitly refuses to touch
    // `current`, and task_wait only ever frees a CHILD of the caller, which by
    // definition is not the caller. If this ever fires, one of those two invariants
    // has been broken and the alternative is dereferencing a freed pointer as the
    // very first act of the switch, so it returns rather than reads.
    if (tasks[current] == NULL) {
        return;
    }

    // EOI ORDERING: the End-Of-Interrupt to the PIC is already sent, by
    // irq_handler (kernel/isr.c), BEFORE it calls this handler. That order is
    // load-bearing here: once we overwrite the pile below and the stub runs iretq
    // into the NEXT task, this handler invocation never returns, so any EOI we
    // tried to send AFTER the switch would never run, the timer line would stay
    // masked, and no further ticks would arrive. The machine would freeze after
    // exactly one switch. Because irq_handler already acked the PIC, the timer
    // keeps firing across the switch. Do not move the EOI after the switch.

    // (1) Save the pile the timer interrupted into the current task's slot.
    tasks[current]->regs = *r;

    // Put the outgoing task back into the rotation ONLY if it was actually running.
    // This used to be an unconditional TASK_READY, which is now wrong: task_block
    // marks the current task TASK_BLOCKED and then drives this very function to
    // switch away, so clobbering the state here would put the task straight back
    // into the rotation and undo the block on the spot, and the "blocked" task
    // would be handed the CPU again a tick later having waited for nothing.
    if (tasks[current]->state == TASK_RUNNING) {
        tasks[current]->state = TASK_READY;
    }

    // (2) Pick the next TASK_READY slot, round-robin, wrapping around. num_tasks
    // (not the MAX_TASKS_LIMIT cap) bounds the walk: it is the high water mark of
    // ids ever handed out, so every task that exists is inside it, and the walk
    // steps over the NULL holes reaping leaves behind.
    int picked = find_next_ready(current);

    // (2a) Nothing is runnable: every task is blocked waiting for something. Do not
    // spin, and do not fall back on a blocked task, which would resume a program in
    // the middle of a wait it has not finished. Sleep the CPU until an interrupt
    // makes someone ready, then ask again, which now succeeds.
    if (picked < 0) {
        idle_until_runnable();
        picked = find_next_ready(current);
    }

    uint32_t next = (uint32_t)picked;
    tasks[next]->state = TASK_RUNNING;

    // If the only ready task is the one we interrupted, there is nothing to switch
    // to. Leave the pile untouched so the stub returns to the same program.
    if (next == current) {
        return;
    }

    current = next;

    // (3) THE SWITCH. Copy the next task's saved pile OVER the live pile in place.
    // This is the whole point and the easiest thing to get wrong: iretq pops its
    // five values (and the stub pops the GPRs) from the STACK, not from this
    // array. Writing tasks[next].regs into a local, or anywhere but through r,
    // would leave the on-stack pile unchanged and iretq would return to the SAME
    // program. It must be written through r, which points at the live stack.
    *r = tasks[next]->regs;

    // (4) SWITCH ADDRESS SPACES. Load the next task's CR3 so its private user half
    // becomes active: the identical VAs (code at 0x400000, stack top 0x800000) now
    // resolve to THIS task's own frames, which is the isolation. Writing CR3 also
    // flushes the TLB (we use no global pages), dropping the previous task's stale
    // user translations for free.
    //
    // ORDERING TRAP: this MUST come after we are done touching the outgoing task's
    // world and before iretq returns to ring 3, and it is only safe mid-interrupt
    // because EVERYTHING the CPU still needs on the way out lives in the KERNEL
    // half, which is cloned identically into every tree: the register pile `r` is
    // on the kernel stack, the tasks[] array and this code are kernel .data/.text,
    // and when the timer next fires the IDT, GDT, TSS rsp0 stack and interrupt
    // stub are all reached through the same kernel mappings. So the switch changes
    // only the user half; the kernel never disappears out from under itself. If
    // any of those lived in the user half this would triple-fault on the next
    // instruction. Use the cached cr3 to avoid a double dereference on this path.
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(tasks[next]->cr3) : "memory");
}
