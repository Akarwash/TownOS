#ifndef ISR_H
#define ISR_H

#include "../include/types.h"

// 64-bit register snapshot handed to the C interrupt handlers. Field names and
// widths are updated for x86-64; the assembly that actually saves them (with
// this exact layout/order) is hand-written in isr_stubs.asm.
typedef struct {
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, user_rsp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *);

void isr_install(void);
void isr_handler(registers_t *regs);
void irq_handler(registers_t *regs);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

#endif