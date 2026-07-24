#ifndef USERMODE_H
#define USERMODE_H

#include "../include/types.h"

// ============================================================================
// Dropping to ring 3 (CPL 3)
// ============================================================================
// x86 never lets you LOWER your privilege with a plain jump or call: the only
// way down is to RETURN into less-privileged code. enter_user_mode forges the
// stack frame an interrupt-return (iretq) expects and executes it, so the CPU
// "returns" into a ring-3 context that never actually ran before.

// RFLAGS for the ring-3 program: bit 1 is reserved-and-always-1, bit 9 (IF) is
// the interrupt flag. IF MUST stay set — MiniOS has no scheduler, so if we
// entered ring 3 with interrupts masked the timer and keyboard would go silent
// and the machine would appear wedged.
#define USER_MODE_RFLAGS  0x202

// Top of the ring-3 stack. The stack lives in PD[3] (0x600000-0x7FFFFF, marked
// PG_USER in boot.asm) and grows DOWN from the top of that 2MB page.
//
// Every task's stack sits at this same virtual address: per-process paging is
// what makes that possible, since each task maps it to its own physical frames.
#define USER_STACK_TOP    0x800000
#define USER_STACK_SIZE   0x40000                        // 256 KB per task
#define USER_STACK_BASE   (USER_STACK_TOP - USER_STACK_SIZE)

// Enter ring 3 at `entry`, with `user_stack_top` as the initial RSP. Does not
// return: control next re-enters the kernel through an interrupt/fault.
void enter_user_mode(uint64_t entry, uint64_t user_stack_top);

#endif
