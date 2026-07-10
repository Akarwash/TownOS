#include "gdt.h"

// TODO(long-mode): 64-bit GDT (HAND-WRITTEN).
//   This whole file is a stub. The final version will:
//     - declare the GDT array (null, kernel code, kernel data descriptors)
//     - fill each descriptor with the correct 64-bit long-mode bit pattern
//     - set gdt_ptr.limit / gdt_ptr.base and call gdt_flush to load it
//   The descriptor contents and the gdt_flush routine are being written by
//   hand, so nothing is generated here.

extern void gdt_flush(uint64_t gdt_ptr_addr);   // defined in gdt_flush.asm (stub)

void gdt_init(void) {
    // TODO(long-mode): build the 64-bit descriptors and call gdt_flush().
}
