#ifndef IDT_H
#define IDT_H

#include "../include/types.h"

// 64-bit IDT gate descriptor: 16 bytes (double the 8-byte protected-mode gate).
// The field LAYOUT below is mechanical/documented, so it is defined here; the
// code that PACKS a handler address into these fields (idt_set_entry) is left
// as a TODO stub.
typedef struct {
    uint16_t offset_low;    // bits 0..15  of the handler address
    uint16_t selector;      // kernel code segment selector
    uint8_t  ist;           // bits 0..2 = Interrupt Stack Table index, rest 0
    uint8_t  flags;         // type + DPL + present (e.g. 0x8E = present, ring 0, interrupt gate)
    uint16_t offset_mid;    // bits 16..31 of the handler address
    uint32_t offset_high;   // bits 32..63 of the handler address
    uint32_t zero;          // reserved, must be 0
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;          // 64-bit linear address of the IDT
} __attribute__((packed)) idt_ptr_t;

void idt_init(void);
void idt_set_entry(int index, uint64_t base, uint16_t selector, uint8_t flags);

#endif
