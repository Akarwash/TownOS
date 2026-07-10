#ifndef GDT_H
#define GDT_H

#include "../include/types.h"

// TODO(long-mode): 64-bit GDT descriptor layout (HAND-WRITTEN).
//   In long mode the segment descriptors are largely ignored (base/limit no
//   longer apply); a code segment is defined mainly by its L (long) and DPL
//   bits, and the data segment by its present/writable bits. The exact 8-byte
//   descriptor bit layout for the 64-bit kernel code/data selectors is being
//   worked out by hand and is intentionally not defined here.
// typedef struct { ... } __attribute__((packed)) gdt_entry_t;

// The GDT pointer (lgdt operand) format IS mechanical: a 16-bit limit followed
// by the 64-bit linear base address of the table.
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

void gdt_init(void);

#endif
