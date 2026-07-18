#ifndef SYSCALLS_H
#define SYSCALLS_H

// ============================================================================
// Syscall numbers: the ABI shared by the kernel and ring-3 programs.
// ============================================================================
// This header is DELIBERATELY standalone: numbers and nothing else, no types,
// no function declarations, no includes. A ring-3 program compiles against it
// without pulling in any kernel code (the kernel's pages are not user-readable,
// so a user program must not depend on anything that lives there).
//
// The convention on the wire: RAX holds the syscall number below; arguments
// follow in RDI, RSI, RDX (System V order); the return value comes back in RAX.
// The vector the program raises is SYSCALL_VECTOR (0x50) from include/vectors.h.

#define SYS_EXIT   0    // no args; stop the machine (no scheduler to return to)
#define SYS_WRITE  1    // RDI = pointer to a NUL-terminated string; print it

#endif
