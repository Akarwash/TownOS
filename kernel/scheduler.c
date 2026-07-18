#include "scheduler.h"
#include "gdt.h"
#include "usermode.h"
#include "heap.h"
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

// User-stack bump allocator.
// A user task's stack MUST be reachable at CPL 3, so it CANNOT come from the
// kernel heap: kmalloc hands out frame-pool pages with no PG_USER bit, and a
// ring-3 push there would page-fault. Stacks are carved instead from PD[3]
// (0x600000-0x800000), the one PG_USER stack page (boot.asm), the same region the
// old hardcoded TASK0/TASK1 stacks split by hand.
//
// TODO(per-process-paging): this is a HARD ceiling and heap-allocating the task
// struct did NOT lift it. Every task's stack still shares this one fixed 2MB
// region, so we get only USER_STACK_COUNT stacks total, and (with no guard pages)
// one task can still run off its slice into a neighbour's. The real fix is
// per-process paging: give each process its own address space and its stacks stop
// competing for one shared region. See docs/reference/memory-map.md.
#define USER_STACK_REGION_START  0x600000     // PD[3] base (6 MB)
#define USER_STACK_REGION_END    0x800000     // top of PD[3] (8 MB)
#define USER_STACK_SIZE          0x40000      // 256 KB per task stack
// (0x800000 - 0x600000) / 0x40000 = 8 stacks total (USER_STACK_COUNT).

// Base of the next stack slice to hand out. Starts at the bottom of the region
// and climbs; each task gets USER_STACK_SIZE and the TOP of its slice as RSP.
static uint64_t next_stack_base = USER_STACK_REGION_START;

// Hand out the next stack slice, returning its TOP (the initial RSP; the stack
// grows DOWN from there). Returns 0 when the region is exhausted, which
// task_create turns into a -1 failure (see the ceiling note above).
static uint64_t alloc_user_stack(void) {
    if (next_stack_base + USER_STACK_SIZE > USER_STACK_REGION_END) {
        return 0;
    }
    uint64_t top = next_stack_base + USER_STACK_SIZE;
    next_stack_base += USER_STACK_SIZE;
    return top;
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

int task_create(uint64_t entry) {
    if (num_tasks >= MAX_TASKS_LIMIT) {
        return -1;                          // bookkeeping array full (arbitrary cap)
    }

    // The task_t is kernel-only bookkeeping, so it is safe on the kernel heap.
    // This is what removes the old fixed-4 ceiling on task structs.
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (t == NULL) {
        return -1;                          // out of heap: same contract as old "table full"
    }

    // The stack is different: ring-3 code pushes to it, so it MUST be user-
    // accessible and therefore cannot come from the kernel heap. Carve it from the
    // PG_USER stack region instead. Do this before bumping num_tasks so a failed
    // create leaves no half-built slot.
    uint64_t stack_top = alloc_user_stack();
    if (stack_top == 0) {
        kfree(t);
        return -1;                          // user-stack region exhausted
    }

    uint32_t id = num_tasks++;
    tasks[id] = t;

    // Forge the pile so iretq will "return" into a program that never ran. This
    // is the exact trick enter_user_mode uses (usermode.c), generalised: instead
    // of pushing the five iretq values by hand and running iretq now, we fill the
    // registers_t the scheduler will later copy onto the stack, and let the
    // timer's iretq consume it.
    memset(&t->regs, 0, sizeof(t->regs));   // all GPRs start at 0
    t->regs.rip = entry;                    // first instruction
    t->regs.user_rsp = stack_top;           // top of this task's stack
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
}
