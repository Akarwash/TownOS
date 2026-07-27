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
    TASK_RUNNING,      // currently on the CPU
    TASK_BLOCKED       // waiting for an event, skipped by the rotation entirely
} task_state_t;

// WHAT a blocked task is waiting for, so the right waker can find it. A task with
// nothing to do is not enough on its own: when a keypress arrives, the keyboard
// IRQ has to be able to pick out the tasks waiting for a KEY and leave alone the
// ones waiting for something else. The reason is that discriminator.
//
// One real reason today. The enum (rather than a bare flag) is the seam where the
// next reasons slot in as the kernel grows: WAIT_CHILD when a parent waits on a
// process to exit, WAIT_DISK when a task waits on a block to arrive. Each new
// reason gets a waker at whatever causes that event.
typedef enum {
    WAIT_NONE = 0,     // not waiting for anything (the only valid value when READY)
    WAIT_KEY           // waiting for a keypress, woken by the keyboard IRQ
} wait_reason_t;

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
    wait_reason_t wait_reason;  // meaningful only while state == TASK_BLOCKED
    uint32_t id;
} task_t;

// Forge a never-run task from a program FILE: read `name` (an 8.3 filename such
// as "A.ELF") off the FAT32 volume, load its ELF segments into a fresh private
// address space, map a fresh stack at the fixed stack VA, and fill its saved
// pile so it looks like it was interrupted at its first instruction (the entry
// point from the file's ELF header), then mark it TASK_READY. Because the
// address space is private, every task's stack sits at the SAME virtual address
// on different physical frames.
//
// Returns the task id, or -1 if the file is missing, is not a program this
// kernel accepts, or the heap or frame pool is out of memory. A failed load
// creates no task and must not disturb the ones that succeeded. Implemented in
// scheduler.c.
int task_create_from_file(const char *name);

// Pick task 0 and enter it. Does not return (control only ever comes back into
// the kernel through an interrupt, where schedule() runs).
void scheduler_start(void);

// Block the current task on `reason` and switch away NOW, rather than waiting for
// the next timer tick. `r` is the live on-stack pile of the syscall the caller is
// serving, the same kind of frame the timer hands schedule().
//
// CALLABLE ONLY FROM A SYSCALL HANDLER. The task is resumed by re-entering the
// syscall from the top (see the re-arm in scheduler.c), which is only meaningful
// for a task that arrived through `int 0x50`. Today the one caller is SYS_READKEY.
//
// This does not return in any useful sense: control leaves through the redirected
// iretq, and this kernel entry is over. Nothing a caller writes after it runs on
// the blocking path.
void task_block(registers_t *r, wait_reason_t reason);

// Make every task blocked on `reason` runnable again. Called by whatever CAUSES
// the event, which today means the keyboard IRQ calling scheduler_wake(WAIT_KEY)
// once it has a key to hand over.
//
// This only changes state; it does NOT switch tasks. A woken task goes back into
// the rotation and the next ordinary schedule() picks it up. That keeps the wake
// safe to call from interrupt context, where switching would be wrong.
void scheduler_wake(wait_reason_t reason);

// The switch itself, called from the timer IRQ with a pointer to the live pile.
// Only TASK_READY tasks are candidates: a blocked task is skipped entirely, and if
// nothing at all is runnable this idles the CPU (see the hlt idle in scheduler.c)
// rather than spinning or resuming a task that is still waiting.
void schedule(registers_t *r);

#endif
