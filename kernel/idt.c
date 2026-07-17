#include "idt.h"
#include "../drivers/ports.h"

idt_entry_t idt[256];
idt_ptr_t idt_ptr;

// No Interrupt Stack Table stack: a value of 0 in the gate's ist field tells the
// CPU to keep using the current stack rather than switching to a preset IST
// stack. We never cross privilege levels (everything runs at ring 0), so the
// running stack is always a valid kernel stack and a dedicated IST is unneeded.
#define IDT_IST_NONE   0

// Pack a 64-bit handler address into one 16-byte long-mode gate. The address is
// scattered across three fields by hardware decree: low 16 bits, middle 16 bits,
// then the upper 32 bits. selector picks the code segment the handler runs in
// (the kernel code selector 0x08, passed in by isr.c); flags carries the
// present bit, DPL, and gate type (0x8E, see isr.c's GATE constant).
void idt_set_entry(int index, uint64_t base, uint16_t selector, uint8_t flags) {
    idt[index].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[index].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[index].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[index].selector    = selector;
    idt[index].ist         = IDT_IST_NONE;
    idt[index].flags       = flags;
    idt[index].zero        = 0;
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
    idt_ptr.base  = (uint64_t)&idt;

    // Zero out the entire IDT (portable).
    uint8_t *ptr = (uint8_t *)&idt;
    uint32_t i;
    for (i = 0; i < sizeof(idt); i++) {
        ptr[i] = 0;
    }

    // Remap the PIC (portable port I/O, unchanged in 64-bit).
    pic_remap();

    // Load the IDT register. This must happen AFTER the kernel GDT is installed
    // (gdt_init runs first in kernel_main): a gate names a code selector, and the
    // CPU resolves that selector against the current GDT the moment an interrupt
    // fires, so the GDT has to be the real one by then.
    //
    // Every gate installed by isr.c is DPL 0. The DPL controls the LOWEST
    // privilege level allowed to reach the gate via a software `int N`. A DPL 3
    // gate would let ring-3 user code execute `int 14` to forge a page fault or
    // `int 32` to fake a timer tick, corrupting kernel state at will. Hardware
    // interrupts and CPU exceptions ignore the DPL, so DPL 0 costs us nothing;
    // only a future syscall vector would deliberately want DPL 3.
    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr));
}