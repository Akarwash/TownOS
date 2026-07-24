#ifndef USERLIB_H
#define USERLIB_H

// ============================================================================
// The entire runtime a MiniOS user program gets.
// ============================================================================
// A user program is now a separately compiled, statically linked ELF64 binary
// that lives on the disk image. It links against NOTHING: no host libc, no
// startup files, and no kernel code. It cannot call a kernel function even if it
// wanted to, because kernel pages are not user-accessible and the call would
// fault. The ONLY channel across the ring boundary is `int 0x50`, the syscall
// gate, and this header is the whole of the user side of it.
//
// The two headers included below are the only things a user program is allowed
// to lean on, and both are deliberately standalone (numbers only, no kernel
// code, no types):
//   include/syscalls.h  the syscall numbers (SYS_WRITE, SYS_EXIT)
//   include/vectors.h   the vector to raise (SYSCALL_VECTOR)

#include "../include/syscalls.h"
#include "../include/vectors.h"

// The raw doorbell. `int $SYSCALL_VECTOR` traps into the kernel's DPL 3 gate.
// Convention (see include/syscalls.h): RAX = syscall number, RDI = first arg,
// return value comes back in RAX. The constraints pin each value to the register
// the ABI names: "a" is RAX, "D" is RDI. SYSCALL_VECTOR reaches the `int`
// instruction as an immediate through the "i" constraint, so the vector stays a
// named constant instead of a bare 0x50 buried in the asm string.
//
// No clobber list is needed beyond memory: the kernel's stub saves and restores
// every GPR from the frame, so on return only RAX has changed. The "memory"
// clobber keeps the compiler from reordering a string's stores after the trap,
// so the kernel sees the finished string.
//
// always_inline is still load-bearing, for a different reason than before. These
// programs build at -O0, where a plain `static inline` is emitted out of line as
// a real function. That was fatal when the helper could land in kernel pages;
// now the program is self-contained so it would merely work. Keeping the inline
// keeps every instruction the program runs inside its own mapped text, with no
// call through a symbol that the (relocation-free) loader would have to resolve.
static inline __attribute__((always_inline))
unsigned long syscall1(unsigned long number, unsigned long arg1) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// SYS_WRITE: hand the kernel a pointer to a NUL-terminated string to print.
// Returns 0 on success, (unsigned long)-1 if the kernel rejected the pointer.
static inline __attribute__((always_inline))
unsigned long sys_write(const char *string) {
    return syscall1(SYS_WRITE, (unsigned long)string);
}

// SYS_EXIT: ask the kernel to halt. Never returns, so neither do we; the
// infinite loop only satisfies the compiler that control does not fall off the
// end.
static inline __attribute__((always_inline))
void sys_exit(void) {
    syscall1(SYS_EXIT, 0);
    for (;;) {
    }
}

// A crude busy-wait so the letters do not scroll past faster than the eye can
// follow. This is NOT a timed delay, just a spin; the count was tuned by eye
// under QEMU for a readable interleave. A real system would sleep, not spin.
#define USER_DELAY_ITERATIONS  20000000

static inline __attribute__((always_inline))
void user_delay(void) {
    // volatile so -O0 (and any optimiser) keeps the loop instead of deleting it.
    for (volatile unsigned long i = 0; i < USER_DELAY_ITERATIONS; i++) {
    }
}

#endif
