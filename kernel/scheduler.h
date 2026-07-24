#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "isr.h"
#include "paging.h"
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
// context to restore; the address space is the memory the task runs in. Each task
// now owns a private page-table tree (per-process paging), so two tasks can use
// the same virtual address for different physical memory. It still has no kernel
// stack of its own (see the limitations in the ADR).
typedef struct {
    registers_t regs;         // the saved/forged interrupt frame: IS the task
    address_space_t *aspace;  // this task's private page-table tree
    uint64_t cr3;             // physical PML4 base to load on switch (== aspace->pml4_phys)
    task_state_t state;
    uint32_t id;
} task_t;

// Forge a never-run task: heap-allocate its task_t, build a private address space
// (copy the ring-3 image to fresh frames mapped at its link address, map a fresh
// stack at the fixed stack VA), fill its saved pile so it looks like it was
// interrupted at its first instruction, mark it TASK_READY. Because the address
// space is private, every task's stack sits at the SAME virtual address on
// different physical frames. Returns the task id, or -1 if the heap or the frame
// pool is out of memory. Implemented in scheduler.c.
int task_create(uint64_t entry);

// The same, for a program that lives on the disk rather than in the kernel
// image: read `name` (an 8.3 filename such as "A.ELF") off the FAT32 volume,
// load its segments into a fresh address space, map a stack, and forge the frame
// with the entry point from the file's ELF header. Returns the task id, or -1 if
// the file is missing, is not a program this kernel accepts, or memory ran out.
// A failed load creates no task and must not disturb the ones that succeeded.
int task_create_from_file(const char *name);

// Pick task 0 and enter it. Does not return (control only ever comes back into
// the kernel through an interrupt, where schedule() runs).
void scheduler_start(void);

// The switch itself, called from the timer IRQ with a pointer to the live pile.
void schedule(registers_t *r);

#endif
