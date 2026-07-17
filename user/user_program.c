// ============================================================================
// The first ring-3 (CPL 3) program in MiniOS.
//
// This code is linked into the .user_text section, which the linker places at
// 0x400000 (4MB). That address lives inside PD[2], the one 2MB page boot.asm
// marks PG_USER, so the CPU will actually let ring-3 code fetch instructions
// from here. Its stack lives in PD[3] (0x600000-0x7FFFFF), also PG_USER.
//
// The program is deliberately tiny and SELF-CONTAINED: it calls nothing in the
// kernel and references no external symbol. It cannot: the kernel's code and
// data pages (PD[0]/PD[1]) are not PG_USER, so a ring-3 call into them would
// fault. Everything it needs, it does inline.
//
// What it proves, in order:
//   1. It runs at all         - a plain local write into its own stack page.
//   2. It is REALLY ring 3     - it executes `cli`, a privileged instruction.
//      At CPL 3 the CPU refuses and raises a #GP (general protection fault,
//      vector 0x0D). THE FAULT IS THE SUCCESS SIGNAL. If we were secretly still
//      in ring 0, `cli` would quietly succeed and we would fall into the spin
//      loop below instead - which is the failure case.
// ============================================================================

__attribute__((section(".user_text")))
void user_program(void) {
    // (1) Prove we are executing in our own pages: write to a local, which the
    // compiler places on our ring-3 stack (PD[3]). `volatile` stops the write
    // from being optimised away as dead.
    volatile int marker = 0;
    marker = 0x1234;
    (void)marker;

    // (2) The privilege test. `cli` clears the interrupt flag and is legal ONLY
    // at CPL 0. From ring 3 it faults with #GP. A correct run never returns from
    // this instruction - control leaves for the kernel's fault handler, which
    // prints CS=0x1B (ring-3 code selector) and halts.
    __asm__ __volatile__("cli");

    // (3) Only reached if `cli` did NOT fault, i.e. we were not really in ring 3.
    // That is a failure of the whole exercise. Spin here so it is obvious (the
    // machine hangs alive instead of faulting) rather than running off into
    // unmapped memory.
    for (;;) {
    }
}
