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

// The linked ring-3 image: everything the bootloader loaded at 0x400000, spanning
// the code (.user_text) and its read-only strings (.user_rodata). These symbols
// come from linker.ld; their ADDRESSES mark the ends of the image. task_create
// copies this whole range into every task's private frames.
extern char _user_text_start[];
extern char _user_rodata_end[];

// Flags for a ring-3 page: present, writable, reachable at CPL 3. Every user page
// (code and stack) is mapped with these; the copied text is left writable for
// simplicity (no W^X in this teaching kernel).
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

// Build the private user half of `as`: copy the linked ring-3 image to fresh
// frames mapped at its link address (0x400000), then map a fresh stack at the
// fixed stack VA. Returns 0 on success, -1 if any frame or page-table allocation
// fails. On failure the frames already mapped into `as` leak, which is acceptable
// here: it only happens on out-of-memory, and tasks are never destroyed.
static int build_user_space(address_space_t *as) {
    // (a) Copy the user image. The bootloader loaded it at physical 0x400000,
    // which the boot tables (active now, before any CR3 switch) identity-map, so
    // _user_text_start is a readable pointer to the original bytes. We copy the
    // WHOLE image (all three programs) into private frames: each task therefore
    // runs its own copy. That is the strongest isolation but the most wasteful.
    // TODO(shared-text): map the read-only text by reference and copy only the
    // writable data, so the three programs share one physical text image.
    uint64_t image_size = (uint64_t)_user_rodata_end - (uint64_t)_user_text_start;
    for (uint64_t off = 0; off < image_size; off += FRAME_SIZE) {
        uint64_t frame = alloc_frame();
        if (frame == 0) {
            return -1;
        }
        // alloc_frame returns an identity-mapped frame (physical == a writable
        // virtual pointer), so both the source (0x400000+off, boot-mapped) and
        // the destination (frame) are writable right now. Copying a whole frame
        // may read a little past the image into the same page, which is harmless.
        memcpy((void *)frame, (void *)((uint64_t)_user_text_start + off), FRAME_SIZE);
        if (paging_map_page(as, (uint64_t)_user_text_start + off, frame, USER_PAGE_FLAGS) != 0) {
            return -1;
        }
    }

    // (b) Map the stack.
    return map_user_stack(as);
}

// Index of the task currently on the CPU.
static uint32_t current = 0;

// How many slots task_create has filled. Tasks are handed ids 0, 1, ... in call
// order and never freed, so this only grows.
static uint32_t num_tasks = 0;

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
    return (int)id;
}

int task_create(uint64_t entry) {
    // Build this task's private address space: its own page-table tree with the
    // kernel cloned in (so interrupts still land in mapped kernel code) and an
    // empty user half. Do all allocation before task_register bumps num_tasks so
    // a failed create leaves no half-built slot for schedule() to trip over.
    address_space_t *as = paging_create_address_space();
    if (as == NULL) {
        return -1;                          // out of frames for the page tables
    }

    // Fill the user half: a private copy of the ring-3 image at 0x400000 and a
    // private stack at the fixed stack VA. Both land on fresh frames unique to
    // this task, which is the whole isolation guarantee.
    if (build_user_space(as) != 0) {
        return -1;                          // out of frames for code/stack pages
    }

    return task_register(as, entry);
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

void schedule(registers_t *r) {
    // Ignore ticks that fire before task 0 has been entered (see the flag above).
    if (!scheduler_running) {
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
    tasks[current]->state = TASK_READY;

    // (2) Pick the next TASK_READY slot, round-robin, wrapping around. Starting at
    // current+1 means we only land back on current (which we just set READY) at
    // i == num_tasks, i.e. when nothing else is runnable. num_tasks (not the
    // MAX_TASKS_LIMIT cap) bounds the walk: tasks are created contiguously and
    // never freed, so slots 0..num_tasks-1 are exactly the live tasks.
    uint32_t next = current;
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t cand = (current + i) % num_tasks;
        if (tasks[cand]->state == TASK_READY) {
            next = cand;
            break;
        }
    }

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
