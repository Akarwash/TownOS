#ifndef ISR_H
#define ISR_H

#include "../include/types.h"

typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *);

void isr_install(void);
void isr_handler(registers_t *regs);
void irq_handler(registers_t *regs);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

#endif