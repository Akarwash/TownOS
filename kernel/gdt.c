#include "gdt.h"

// ============================================================================
// 64-bit kernel GDT + TSS
// ============================================================================
// boot/boot.asm brought us into long mode with a minimal BOOTSTRAP GDT whose
// selectors are: kernel code = 0x08, kernel data = 0x10. This file installs the
// real kernel GDT and MUST keep those two selectors meaning the same thing, or
// the CS reload inside gdt_flush would fault. We extend the table with user
// code/data descriptors and a TSS for the user-mode work coming later.
//
// Selector layout (index * 8, low bits = RPL):
//   0x00  null
//   0x08  kernel code (index 1, DPL 0)
//   0x10  kernel data (index 2, DPL 0)
//   0x1B  user code   (index 3, DPL 3 -> 0x18 | 3)
//   0x23  user data   (index 4, DPL 3 -> 0x20 | 3)
//   0x28  TSS         (indices 5+6, the 16-byte system descriptor)

extern void gdt_flush(uint64_t gdt_ptr_addr);   // defined in gdt_flush.asm

// The GDT is declared as an array of 8-byte entries. The TSS needs a 16-byte
// system descriptor, so it occupies slots 5 AND 6; we write it there by casting
// &gdt[5] to tss_descriptor_t*. (Chosen over a raw byte array so the ordinary
// code/data slots stay type-checked.)
#define GDT_ENTRIES 7
static gdt_entry_t gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static gdt_ptr_t   gdt_ptr;

// The TSS itself, plus a dedicated 16KB ring-0 stack, both in .bss. This stack
// is SEPARATE from the boot stack in boot.asm — it is the stack the CPU switches
// to (via tss.rsp0) when an interrupt crosses from user mode into the kernel.
static tss_t   tss;
static uint8_t tss_stack[16384] __attribute__((aligned(16)));

// Fill one standard 8-byte descriptor. base/limit are ignored by the CPU for
// long-mode code/data, but we still write them so the format is correct.
static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[i].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access      = access;
    // low nibble = limit bits 16..19, high nibble = flags (G, D/B, L, AVL)
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
}

// Write the 16-byte TSS descriptor across slots 5 and 6.
static void gdt_set_tss(int i, uint64_t base, uint32_t limit) {
    tss_descriptor_t *td = (tss_descriptor_t *)&gdt[i];
    td->low.limit_low   = (uint16_t)(limit & 0xFFFF);
    td->low.base_low    = (uint16_t)(base & 0xFFFF);
    td->low.base_middle = (uint8_t)((base >> 16) & 0xFF);
    td->low.access      = GDT_ACCESS_TSS;          // 0x89, available 64-bit TSS
    td->low.granularity = (uint8_t)((limit >> 16) & 0x0F);  // byte granularity, no flags
    td->low.base_high   = (uint8_t)((base >> 24) & 0xFF);
    td->base_upper      = (uint32_t)((base >> 32) & 0xFFFFFFFF);  // the extra 32 base bits
    td->reserved        = 0;
}

void gdt_init(void) {
    // Zero the TSS explicitly — we do not rely on the bootloader having cleared
    // .bss. Only rsp0 and iomap_base need non-zero values.
    uint8_t *p = (uint8_t *)&tss;
    for (size_t n = 0; n < sizeof(tss); n++) {
        p[n] = 0;
    }
    // rsp0 = TOP of the ring-0 stack (stacks grow down).
    tss.rsp0 = (uint64_t)(tss_stack + sizeof(tss_stack));
    // iomap_base == sizeof(tss_t) means "no I/O permission bitmap".
    tss.iomap_base = (uint16_t)sizeof(tss_t);

    // 0: null descriptor.
    gdt_set_entry(0, 0, 0, 0, 0);
    // 1: kernel code — L=1, D/B=0.
    gdt_set_entry(1, 0, 0, GDT_ACCESS_KCODE, GDT_FLAGS_KCODE);
    // 2: kernel data.
    gdt_set_entry(2, 0, 0, GDT_ACCESS_KDATA, GDT_FLAGS_KDATA);
    // 3: user code — same as kernel code but DPL 3.
    gdt_set_entry(3, 0, 0, GDT_ACCESS_UCODE, GDT_FLAGS_KCODE);
    // 4: user data — DPL 3.
    gdt_set_entry(4, 0, 0, GDT_ACCESS_UDATA, GDT_FLAGS_KDATA);
    // 5+6: TSS descriptor. limit = last valid byte of the TSS.
    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss_t) - 1);

    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint64_t)&gdt;

    // Load the GDT and reload every segment register (see gdt_flush.asm).
    gdt_flush((uint64_t)&gdt_ptr);

    // Load the task register with the TSS selector (0x28). Done here as a one-
    // instruction inline asm rather than in gdt_flush.asm: it is trivial and
    // keeps the assembly file focused on the non-obvious CS-reload trick.
    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)0x28));

    // NOTE: the TSS is inert for now. Everything runs at CPL 0, so no interrupt
    // changes privilege level, so the CPU never consults tss.rsp0. It exists so
    // that when user mode (ring 3) arrives, ring-0 entry has a known-good stack.
}
