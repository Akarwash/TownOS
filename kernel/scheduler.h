#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "isr.h"
#include "../include/types.h"

// ============================================================================
// A round-robin preemptive scheduler.
// ============================================================================
// The trick that makes this work: the register pile the interrupt stub pushes
// onto the kernel stack (a registers_t, see isr.h) IS the task. It holds every
// GPR plus the rip/cs/rflags/rsp/ss that iretq will restore. To switch tasks we
// (1) save the interrupted pile into the current task's slot, then (2) copy a
// DIFFERENT task's slot over the SAME live pile, so the stub restores what it
// thinks is the same program but is really the next one. See schedule() and
// docs/reference/scheduling.md.

// Fixed task table: no dynamic allocation. The frame allocator (kernel/memory.c)
// hands out addresses above the 8M identity map that fault on first touch, and
// there is no kernel heap. TODO: make this dynamic once a real allocator exists.
#define MAX_TASKS 4

typedef enum {
    TASK_UNUSED = 0,   // slot never filled (.bss zero-init lands here)
    TASK_READY,        // runnable, waiting for its slice
    TASK_RUNNING       // currently on the CPU
} task_state_t;

// One task is its saved register pile plus a little bookkeeping. The pile is the
// whole context; there is nothing else to a task in this kernel (no address
// space of its own, no kernel stack of its own; see the limitations in the ADR).
typedef struct {
    registers_t regs;    // the saved/forged interrupt frame: IS the task
    task_state_t state;
    uint32_t id;
} task_t;

// Crude static split of PD[3] (0x600000-0x7FFFFF, the one PG_USER stack page).
// Each stack grows DOWN from its top, giving each task 1MB. This is a stopgap:
// a real system allocates a stack per task from an allocator, not by carving a
// single hard-coded page in half. See docs/reference/memory-map.md.
#define TASK0_STACK_TOP  0x700000
#define TASK1_STACK_TOP  0x800000

// Forge a never-run task: fill its saved pile so it looks like it was interrupted
// at its first instruction, mark it TASK_READY. Returns the task id, or -1 if the
// table is full.
int task_create(uint64_t entry, uint64_t stack_top);

// Pick task 0 and enter it. Does not return (control only ever comes back into
// the kernel through an interrupt, where schedule() runs).
void scheduler_start(void);

// The switch itself, called from the timer IRQ with a pointer to the live pile.
void schedule(registers_t *r);

#endif
