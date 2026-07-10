#include "isr.h"
#include "idt.h"
#include "../drivers/screen.h"
#include "../drivers/ports.h"

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static isr_handler_t interrupt_handlers[256];

#define KCODE 0x08      // kernel code segment selector
#define GATE  0x8E      // present, ring 0, 64-bit interrupt gate

void isr_install(void) {
    // Zero the IDT, remap the PIC, and load the IDT register first.
    idt_init();

    idt_set_entry(0,  (uint64_t)isr0,  KCODE, GATE);
    idt_set_entry(1,  (uint64_t)isr1,  KCODE, GATE);
    idt_set_entry(2,  (uint64_t)isr2,  KCODE, GATE);
    idt_set_entry(3,  (uint64_t)isr3,  KCODE, GATE);
    idt_set_entry(4,  (uint64_t)isr4,  KCODE, GATE);
    idt_set_entry(5,  (uint64_t)isr5,  KCODE, GATE);
    idt_set_entry(6,  (uint64_t)isr6,  KCODE, GATE);
    idt_set_entry(7,  (uint64_t)isr7,  KCODE, GATE);
    idt_set_entry(8,  (uint64_t)isr8,  KCODE, GATE);
    idt_set_entry(9,  (uint64_t)isr9,  KCODE, GATE);
    idt_set_entry(10, (uint64_t)isr10, KCODE, GATE);
    idt_set_entry(11, (uint64_t)isr11, KCODE, GATE);
    idt_set_entry(12, (uint64_t)isr12, KCODE, GATE);
    idt_set_entry(13, (uint64_t)isr13, KCODE, GATE);
    idt_set_entry(14, (uint64_t)isr14, KCODE, GATE);
    idt_set_entry(15, (uint64_t)isr15, KCODE, GATE);
    idt_set_entry(16, (uint64_t)isr16, KCODE, GATE);
    idt_set_entry(17, (uint64_t)isr17, KCODE, GATE);
    idt_set_entry(18, (uint64_t)isr18, KCODE, GATE);
    idt_set_entry(19, (uint64_t)isr19, KCODE, GATE);
    idt_set_entry(20, (uint64_t)isr20, KCODE, GATE);
    idt_set_entry(21, (uint64_t)isr21, KCODE, GATE);
    idt_set_entry(22, (uint64_t)isr22, KCODE, GATE);
    idt_set_entry(23, (uint64_t)isr23, KCODE, GATE);
    idt_set_entry(24, (uint64_t)isr24, KCODE, GATE);
    idt_set_entry(25, (uint64_t)isr25, KCODE, GATE);
    idt_set_entry(26, (uint64_t)isr26, KCODE, GATE);
    idt_set_entry(27, (uint64_t)isr27, KCODE, GATE);
    idt_set_entry(28, (uint64_t)isr28, KCODE, GATE);
    idt_set_entry(29, (uint64_t)isr29, KCODE, GATE);
    idt_set_entry(30, (uint64_t)isr30, KCODE, GATE);
    idt_set_entry(31, (uint64_t)isr31, KCODE, GATE);

    idt_set_entry(32, (uint64_t)irq0,  KCODE, GATE);
    idt_set_entry(33, (uint64_t)irq1,  KCODE, GATE);
    idt_set_entry(34, (uint64_t)irq2,  KCODE, GATE);
    idt_set_entry(35, (uint64_t)irq3,  KCODE, GATE);
    idt_set_entry(36, (uint64_t)irq4,  KCODE, GATE);
    idt_set_entry(37, (uint64_t)irq5,  KCODE, GATE);
    idt_set_entry(38, (uint64_t)irq6,  KCODE, GATE);
    idt_set_entry(39, (uint64_t)irq7,  KCODE, GATE);
    idt_set_entry(40, (uint64_t)irq8,  KCODE, GATE);
    idt_set_entry(41, (uint64_t)irq9,  KCODE, GATE);
    idt_set_entry(42, (uint64_t)irq10, KCODE, GATE);
    idt_set_entry(43, (uint64_t)irq11, KCODE, GATE);
    idt_set_entry(44, (uint64_t)irq12, KCODE, GATE);
    idt_set_entry(45, (uint64_t)irq13, KCODE, GATE);
    idt_set_entry(46, (uint64_t)irq14, KCODE, GATE);
    idt_set_entry(47, (uint64_t)irq15, KCODE, GATE);

    // Handlers are in place; now enable hardware interrupts.
    __asm__ __volatile__("sti");
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_handler(registers_t *regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    } else {
        print_string("Unhandled interrupt: ");
        print_int(regs->int_no);
        print_char('\n');
    }
}

void irq_handler(registers_t *regs) {
    // Acknowledge the PIC(s). Slave first if the IRQ came from it (int >= 40).
    if (regs->int_no >= 40) {
        port_byte_out(0xA0, 0x20);
    }
    port_byte_out(0x20, 0x20);

    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }
}
