#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

// ============================================================================
// The syscall handler (kernel side of the int 0x50 doorbell).
// ============================================================================
// syscall_stub (kernel/isr_stubs.asm) is the entry point named by the one DPL 3
// IDT gate at SYSCALL_VECTOR. It builds the same registers_t frame every other
// interrupt stub builds and calls syscall_handler with a pointer to it.
//
// The call number is in regs->rax; arguments in regs->rdi, regs->rsi, regs->rdx
// (System V order). The return value is written back into regs->rax, so when the
// stub restores registers and executes iretq, the caller finds it in RAX.
void syscall_handler(registers_t *regs);

#endif
