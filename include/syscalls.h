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

#define SYS_EXIT     0   // RDI = exit status (masked to 0..255); ends the calling task. Never returns.
#define SYS_WRITE    1   // RDI = pointer to a NUL-terminated string; print it
#define SYS_READKEY  2   // no args; return one buffered key in RAX, or 0 if none
#define SYS_LIST     3   // RDI = buffer, RSI = size; list root dir names, return count
#define SYS_RUN      4   // RDI = filename ptr; load and start it, return 0 or -1
#define SYS_READFILE 5   // RDI = filename ptr, RSI = buffer, RDX = size; return bytes read
#define SYS_WAIT     6   // no args; block until any child exits; RAX = that child's exit status, or SYSCALL_ERROR if the caller has no children

#endif
