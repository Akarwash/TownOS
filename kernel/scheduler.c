#include "scheduler.h"
#include "gdt.h"
#include "usermode.h"
#include "../libc/mem.h"

// The task table lives in .bss, so it is zero-initialised: every slot starts
// TASK_UNUSED (== 0) with a zeroed pile. No kernel heap exists to allocate it
// dynamically (see the note in scheduler.h).
static task_t tasks[MAX_TASKS];

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

int task_create(uint64_t entry, uint64_t stack_top) {
    if (num_tasks >= MAX_TASKS) {
        return -1;
    }

    uint32_t id = num_tasks++;
    task_t *t = &tasks[id];

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
    tasks[0].state = TASK_RUNNING;
    scheduler_running = 1;

    // Reuse the proven ring-3 entry path rather than hand-rolling a second iretq.
    // Task 0's forged GPRs (all zero) do not matter on this first entry: a fresh
    // program sets up its own registers before it reads any. Every LATER entry
    // into task 0 goes through schedule(), which restores its full saved pile.
    enter_user_mode(tasks[0].regs.rip, tasks[0].regs.user_rsp);
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
    tasks[current].regs = *r;
    tasks[current].state = TASK_READY;

    // (2) Pick the next TASK_READY slot, round-robin, wrapping around. Starting at
    // current+1 means we only land back on current (which we just set READY) at
    // i == MAX_TASKS, i.e. when nothing else is runnable.
    uint32_t next = current;
    for (uint32_t i = 1; i <= MAX_TASKS; i++) {
        uint32_t cand = (current + i) % MAX_TASKS;
        if (tasks[cand].state == TASK_READY) {
            next = cand;
            break;
        }
    }

    tasks[next].state = TASK_RUNNING;

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
    *r = tasks[next].regs;
}
