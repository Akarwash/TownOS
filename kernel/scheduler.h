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

// A generous, arbitrary cap on how many tasks we TRACK. This is NOT the old
// storage ceiling: the task_t structs are now heap-allocated (kmalloc, see
// task_create), so the kernel heap that used to be missing exists. This only
// bounds the size of the pointer-bookkeeping array in scheduler.c, not where the
// tasks live. 64 is arbitrary; raise it freely.
#define MAX_TASKS_LIMIT 64

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

// Forge a never-run task: heap-allocate its task_t, ask the user-stack allocator
// (scheduler.c) for a stack, fill its saved pile so it looks like it was
// interrupted at its first instruction, mark it TASK_READY. task_create no longer
// takes a stack: it hands out one from the PG_USER stack region itself, because a
// ring-3 stack cannot live on the kernel heap (see scheduler.c). Returns the task
// id, or -1 if the heap is out of memory or the user-stack region is exhausted.
int task_create(uint64_t entry);

// Pick task 0 and enter it. Does not return (control only ever comes back into
// the kernel through an interrupt, where schedule() runs).
void scheduler_start(void);

// The switch itself, called from the timer IRQ with a pointer to the live pile.
void schedule(registers_t *r);

#endif
