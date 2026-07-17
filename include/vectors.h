#ifndef VECTORS_H
#define VECTORS_H

// ============================================================================
// The single source of truth for every interrupt vector number in MiniOS.
//
// After this header exists, a bare vector number must not appear anywhere else
// in the codebase. Every consumer (the IDT gate installer, the PIC remap, the
// interrupt stubs, the drivers) refers to these names instead.
//
// The vector map is SELF-DESCRIBING and a deliberate, documented deviation from
// x86 convention (see docs/decisions/0005-self-describing-vector-map.md):
//
//   0x00 - 0x1F   CPU exceptions   Intel's, forced, cannot be moved
//   0x40 - 0x4F   hardware IRQs    "4 = the world poked us"
//   0x50 - 0x5F   syscalls         "5 = a program is asking for a favour"
//
// The category is now legible at a glance from the high nibble: a 0x4? in a
// fault log is a device, a 0x5? is a program asking, a 0x0?/0x1? is something
// breaking. The conventional 0x20 IRQ base carries no such information.
//
// CONSTRAINT: a PIC base MUST be a multiple of 8. The 8259's ICW2 latches only
// the upper five bits of the base and substitutes the IRQ line number into the
// low three, so a base that is not a multiple of 8 would alias several IRQs
// onto the same vector. 0x40 and 0x48 satisfy this; do not pick a base that
// does not. Both are also above Intel's reserved 0x00 - 0x1F range.
// ============================================================================

// --- CPU exceptions (vectors 0x00 - 0x1F, fixed by Intel) -------------------
// A #DF-style short name in a comment is the Intel mnemonic; the constant name
// is MiniOS's own. Vectors marked RESERVED are defined so the range is complete
// and so a stray one still resolves to a name, not a bare number.
#define EXC_DIVIDE_BY_ZERO          0   // #DE  divide or modulo by zero
#define EXC_DEBUG                   1   // #DB  debug trap / single step
#define EXC_NON_MASKABLE            2   //      NMI, an unmaskable hardware line
#define EXC_BREAKPOINT              3   // #BP  int3, a planted breakpoint
#define EXC_OVERFLOW                4   // #OF  into with the overflow flag set
#define EXC_BOUND_RANGE             5   // #BR  bound check out of range
#define EXC_INVALID_OPCODE          6   // #UD  bytes that are not an instruction
#define EXC_DEVICE_NOT_AVAILABLE    7   // #NM  FPU/SSE used while disabled
#define EXC_DOUBLE_FAULT            8   // #DF  a fault raised while handling one
#define EXC_COPROCESSOR_OVERRUN     9   //      legacy, unused on modern CPUs
#define EXC_INVALID_TSS            10   // #TS  a bad TSS selector
#define EXC_SEGMENT_NOT_PRESENT    11   // #NP  a segment with present bit clear
#define EXC_STACK_SEGMENT_FAULT    12   // #SS  a stack-segment access fault
#define EXC_GENERAL_PROTECTION     13   // #GP  a forbidden operation, catch-all
#define EXC_PAGE_FAULT             14   // #PF  a virtual address that could not map
#define EXC_RESERVED_15           15   //      reserved by Intel
#define EXC_X87_FLOATING_POINT     16   // #MF  a pending x87 FPU error
#define EXC_ALIGNMENT_CHECK        17   // #AC  a misaligned access with checks on
#define EXC_MACHINE_CHECK          18   // #MC  the hardware reported an internal fault
#define EXC_SIMD_FLOATING_POINT    19   // #XM  an SSE/AVX floating-point error
#define EXC_VIRTUALIZATION         20   // #VE  an EPT violation delivered to the guest
#define EXC_CONTROL_PROTECTION     21   // #CP  a shadow-stack / CET violation
#define EXC_RESERVED_22           22   //      reserved by Intel
#define EXC_RESERVED_23           23   //      reserved by Intel
#define EXC_RESERVED_24           24   //      reserved by Intel
#define EXC_RESERVED_25           25   //      reserved by Intel
#define EXC_RESERVED_26           26   //      reserved by Intel
#define EXC_RESERVED_27           27   //      reserved by Intel
#define EXC_HYPERVISOR_INJECTION   28   // #HV  hypervisor injection exception
#define EXC_VMM_COMMUNICATION      29   // #VC  SEV-ES VMM communication
#define EXC_SECURITY               30   // #SX  a security-sensitive event
#define EXC_RESERVED_31           31   //      reserved by Intel

#define EXC_COUNT                  32   // exceptions occupy 0x00 - 0x1F

// --- Hardware IRQs (vectors 0x40 - 0x4F, remapped off the reserved range) ----
// The two PIC bases. IRQ0 through IRQ7 arrive relative to the master base,
// IRQ8 through IRQ15 relative to the slave base.
#define PIC_MASTER_VECTOR_BASE   0x40
#define PIC_SLAVE_VECTOR_BASE    0x48

// Named IRQ vectors, derived from the bases rather than hardcoded, so moving a
// base moves the whole block. IRQ2 is the cascade line and never fires as a
// device, but its vector exists for completeness.
#define IRQ_TIMER        (PIC_MASTER_VECTOR_BASE + 0)   // 0x40  PIT channel 0
#define IRQ_KEYBOARD     (PIC_MASTER_VECTOR_BASE + 1)   // 0x41  PS/2 keyboard
#define IRQ_CASCADE      (PIC_MASTER_VECTOR_BASE + 2)   // 0x42  slave PIC cascade
#define IRQ_COM2         (PIC_MASTER_VECTOR_BASE + 3)   // 0x43
#define IRQ_COM1         (PIC_MASTER_VECTOR_BASE + 4)   // 0x44
#define IRQ_LPT2         (PIC_MASTER_VECTOR_BASE + 5)   // 0x45
#define IRQ_FLOPPY       (PIC_MASTER_VECTOR_BASE + 6)   // 0x46
#define IRQ_LPT1         (PIC_MASTER_VECTOR_BASE + 7)   // 0x47  (spurious on master)
#define IRQ_RTC          (PIC_SLAVE_VECTOR_BASE  + 0)   // 0x48  real-time clock
#define IRQ_9            (PIC_SLAVE_VECTOR_BASE  + 1)   // 0x49
#define IRQ_10           (PIC_SLAVE_VECTOR_BASE  + 2)   // 0x4A
#define IRQ_11           (PIC_SLAVE_VECTOR_BASE  + 3)   // 0x4B
#define IRQ_MOUSE        (PIC_SLAVE_VECTOR_BASE  + 4)   // 0x4C  PS/2 mouse
#define IRQ_13           (PIC_SLAVE_VECTOR_BASE  + 5)   // 0x4D  FPU / coprocessor
#define IRQ_ATA_PRIMARY  (PIC_SLAVE_VECTOR_BASE  + 6)   // 0x4E  primary ATA
#define IRQ_15           (PIC_SLAVE_VECTOR_BASE  + 7)   // 0x4F  secondary ATA

// --- Syscalls (vectors 0x50 - 0x5F) -----------------------------------------
// Reserved now, wired up later. Nothing installs a gate here yet; defining the
// vector claims the number so a future syscall path has a name to grow into.
#define SYSCALL_VECTOR           0x50

#endif
