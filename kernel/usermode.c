#include "usermode.h"
#include "gdt.h"

// ============================================================================
// The ring-3 transition.
// ============================================================================
// There is no "enter ring 3" instruction. The CPU only DROPS privilege on a
// return into less-privileged code, so we hand-build the exact stack frame that
// iretq consumes and let it "return" into a context that never really ran.
//
// In 64-bit mode iretq pops five 8-byte values, in this order:
//   RIP, CS, RFLAGS, RSP, SS
// so we push them in the REVERSE order (SS first, RIP last). Because the CS we
// supply has RPL 3, the CPU performs a privilege change: it switches to the
// pushed SS:RSP and begins executing at CS:RIP as ring-3 code.
void enter_user_mode(uint64_t entry, uint64_t user_stack_top) {
    __asm__ __volatile__(
        // Point the data segment registers at the ring-3 data selector. Long
        // mode mostly ignores these for addressing, but leaving ring-0 selectors
        // loaded across a privilege drop is untidy and can fault on some ops.
        "movw %[udata], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        // Forge the iretq frame (pushed high-to-low so iretq pops them right):
        "pushq %[udata]\n\t"   // SS      = ring-3 data selector
        "pushq %[stack]\n\t"   // RSP     = top of the ring-3 stack
        "pushq %[flags]\n\t"   // RFLAGS  = IF set, so interrupts stay live
        "pushq %[ucode]\n\t"   // CS      = ring-3 code selector (RPL 3 -> the drop)
        "pushq %[entry]\n\t"   // RIP     = user program entry point
        "iretq\n\t"            // "return" into CPL 3 and start executing there
        :
        : [udata] "i"(GDT_SELECTOR_USER_DATA),
          [ucode] "i"(GDT_SELECTOR_USER_CODE),
          [flags] "i"(USER_MODE_RFLAGS),
          [stack] "r"(user_stack_top),
          [entry] "r"(entry)
        : "rax", "memory");
}
