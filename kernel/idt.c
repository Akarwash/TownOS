#include "idt.h"
#include "../drivers/ports.h"

idt_entry_t idt[256];
idt_ptr_t idt_ptr;

void idt_set_entry(int index, uint32_t base, uint16_t selector, uint8_t flags) {
    idt[index].base_low  = base & 0xFFFF;
    idt[index].base_high = (base >> 16) & 0xFFFF;
    idt[index].selector  = selector;
    idt[index].always0   = 0;
    idt[index].flags     = flags;
}

static void pic_remap(void) {
    // Save current masks
    uint8_t mask1 = port_byte_in(0x21);
    uint8_t mask2 = port_byte_in(0xA1);

    // ICW1: start initialization sequence
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);

    // ICW2: set interrupt vector offsets
    port_byte_out(0x21, 0x20);    // master PIC: IRQs start at interrupt 32 (0x20)
    port_byte_out(0xA1, 0x28);    // slave PIC: IRQs start at interrupt 40 (0x28)

    // ICW3: tell PICs about each other
    port_byte_out(0x21, 0x04);    // master: slave is on IRQ 2 (bit 2 set)
    port_byte_out(0xA1, 0x02);    // slave: your cascade identity is 2

    // ICW4: set mode
    port_byte_out(0x21, 0x01);    // 8086 mode
    port_byte_out(0xA1, 0x01);    // 8086 mode

    // Restore saved masks
    port_byte_out(0x21, mask1);
    port_byte_out(0xA1, mask2);
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;

    // Zero out the entire IDT
    uint8_t *ptr = (uint8_t *)&idt;
    uint32_t i;
    for (i = 0; i < sizeof(idt); i++) {
        ptr[i] = 0;
    }

    // Remap the PIC
    pic_remap();

    // Load the IDT
    __asm__ __volatile__("lidt (%0)" : : "r" (&idt_ptr));
}