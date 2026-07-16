#ifndef GDT_H
#define GDT_H

#include "../include/types.h"

// ============================================================================
// 64-bit Global Descriptor Table (GDT) + Task State Segment (TSS)
// ============================================================================
// In long mode the segment descriptors are largely ignored: base and limit no
// longer bound memory accesses (paging does protection instead). A code segment
// is defined mainly by its L (long) and DPL bits, a data segment by its
// present/writable bits. The 8-byte descriptor FORMAT is nonetheless fixed by
// hardware, so every field below must still exist even though the CPU ignores
// most of them for code/data.

// --- Access byte (bit 7 -> 0): P | DPL(2) | S | E | DC | RW | A ---
#define GDT_ACCESS_PRESENT   (1 << 7)   // P:  segment is present
#define GDT_ACCESS_DPL0      (0 << 5)   // DPL 0: kernel/ring 0
#define GDT_ACCESS_DPL3      (3 << 5)   // DPL 3: user/ring 3
#define GDT_ACCESS_SEGMENT   (1 << 4)   // S:  1 = code/data segment (0 = system, e.g. TSS)
#define GDT_ACCESS_EXEC      (1 << 3)   // E:  1 = code, 0 = data
#define GDT_ACCESS_RW        (1 << 1)   // RW: code -> readable, data -> writable

// Assembled access bytes for each descriptor kind:
#define GDT_ACCESS_KCODE  0x9A   // P=1 DPL=0 S=1 E=1 DC=0 RW=1 A=0  (kernel code)
#define GDT_ACCESS_KDATA  0x92   // P=1 DPL=0 S=1 E=0 DC=0 RW=1 A=0  (kernel data)
#define GDT_ACCESS_UCODE  0xFA   // P=1 DPL=3 S=1 E=1 DC=0 RW=1 A=0  (user code)
#define GDT_ACCESS_UDATA  0xF2   // P=1 DPL=3 S=1 E=0 DC=0 RW=1 A=0  (user data)
#define GDT_ACCESS_TSS    0x89   // P=1 DPL=0 S=0 type=9 (available 64-bit TSS)

// --- Flags nibble (granularity high nibble, bit 7 -> 4): G | D/B | L | AVL ---
#define GDT_FLAG_GRAN     (1 << 7)   // G:   limit is in 4KB pages
#define GDT_FLAG_DB       (1 << 6)   // D/B: 32-bit segment. MUST be 0 when L=1.
#define GDT_FLAG_LONG     (1 << 5)   // L:   64-bit code segment

// A 64-bit code segment sets L and clears D/B (setting both is illegal).
// Data segments in long mode carry no meaningful flags, so their nibble is 0.
#define GDT_FLAGS_KCODE   (GDT_FLAG_LONG)   // L=1, D/B=0
#define GDT_FLAGS_KDATA   (0)

// --- Standard 8-byte descriptor ---
// The base/limit fields are dead weight in long mode for code/data, but the
// layout is fixed by hardware so they must be present. They ARE meaningful for
// the system (TSS) descriptor below.
typedef struct {
    uint16_t limit_low;     // limit bits 0..15
    uint16_t base_low;      // base bits 0..15
    uint8_t  base_middle;   // base bits 16..23
    uint8_t  access;        // P | DPL | S | E | DC | RW | A
    uint8_t  granularity;   // limit bits 16..19 (low nibble) | flags (high nibble)
    uint8_t  base_high;     // base bits 24..31
} __attribute__((packed)) gdt_entry_t;

// --- 16-byte TSS (system) descriptor ---
// A 64-bit TSS lives at a 64-bit linear address, which does not fit in the
// legacy 32-bit base of an 8-byte descriptor. The system descriptor is therefore
// an 8-byte gdt_entry_t extended with the upper 32 base bits, so it spans two
// GDT slots.
typedef struct {
    gdt_entry_t low;        // reuses the 8-byte layout for the low 32 base bits
    uint32_t    base_upper; // base bits 32..63
    uint32_t    reserved;   // must be zero
} __attribute__((packed)) tss_descriptor_t;

// --- 64-bit Task State Segment (104 bytes) ---
// In long mode the TSS no longer holds a task context; it mainly provides the
// stack pointers the CPU switches to on a privilege-level change (rsp0..rsp2)
// and the interrupt-stack-table entries (ist1..ist7).
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          // stack loaded on an interrupt that enters ring 0
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;          // interrupt stack table entries (unused for now)
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    // offset to the I/O permission bitmap
} __attribute__((packed)) tss_t;

// Packing mistakes here would silently corrupt the descriptor the CPU reads, so
// assert the hardware-mandated size at compile time.
_Static_assert(sizeof(tss_t) == 104, "tss_t must be exactly 104 bytes");

// The GDT pointer (lgdt operand) format IS mechanical: a 16-bit limit followed
// by the 64-bit linear base address of the table.
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

void gdt_init(void);

#endif
