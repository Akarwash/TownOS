#include "scheduler.h"
#include "gdt.h"
#include "usermode.h"
#include "heap.h"
#include "paging.h"
#include "memory.h"
#include "elf.h"
#include "../libc/mem.h"

// Pointers to the heap-allocated tasks, in creation order (ids 0, 1, ...). Slots
// 0..num_tasks-1 are non-NULL; the rest are unused. Each task_t is kmalloc'd in
// task_create. A flat pointer array (rather than a linked list) keeps schedule()'s
// O(1) round-robin indexing a mechanical change from the old fixed array.
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

// How many slots task_create has filled. Tasks are handed ids 0, 1, ... in call
// order and never freed, so this only grows.
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
// Returns the task id, or -1 if the heap is full or the task table is.
static int task_register(address_space_t *as, uint64_t entry) {
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
    return (int)id;
}

int task_create_from_file(const char *name) {
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

    return task_register(as, entry);
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
// and so cannot be handed back the CPU it just gave up.
static int find_next_ready(uint32_t from) {
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t cand = (from + i) % num_tasks;
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
    // (not the MAX_TASKS_LIMIT cap) bounds the walk: tasks are created contiguously
    // and never freed, so slots 0..num_tasks-1 are exactly the live tasks.
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
