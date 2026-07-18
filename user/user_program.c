// ============================================================================
// The first ring-3 (CPL 3) program in MiniOS that asks the kernel for something.
//
// This code is linked into the .user_text section, which the linker places at
// 0x400000 (4MB). That address lives inside PD[2], the one 2MB page boot.asm
// marks PG_USER, so the CPU will actually let ring-3 code fetch instructions
// from here. Its stack lives in PD[3] (0x600000-0x7FFFFF), also PG_USER.
//
// The program still references NOTHING in the kernel: it cannot call a kernel
// function (those pages are not PG_USER, the call would fault) and it cannot
// read a kernel address (same reason). The ONLY channel across the ring
// boundary is `int 0x50`, the syscall gate. Everything else is inline.
//
// What it proves, in order:
//   1. A ring-3 program can cross into the kernel through the syscall gate:
//      SYS_WRITE prints a string the kernel reads back out of the user region.
//   2. It can do it more than once: a second SYS_WRITE with a different string.
//   3. It can ask the kernel to stop the machine: SYS_EXIT, which never returns.
//
// The two headers below are the ONLY things a user program is allowed to lean
// on, and both are deliberately standalone (numbers only, no kernel code):
//   include/syscalls.h  the syscall numbers (SYS_WRITE, SYS_EXIT)
//   include/vectors.h   the vector to raise (SYSCALL_VECTOR)
// types.h is just the fixed-width integer typedefs, no code either.
// ============================================================================

#include "../include/types.h"
#include "../include/syscalls.h"
#include "../include/vectors.h"

// The raw doorbell. `int $SYSCALL_VECTOR` traps into the kernel's DPL 3 gate.
// Convention (see include/syscalls.h): RAX = syscall number, RDI = first arg,
// return value comes back in RAX. The constraints below pin each value to the
// register the ABI names: "a" is RAX, "D" is RDI. SYSCALL_VECTOR reaches the
// `int` instruction as an immediate through the "i" constraint, so the vector
// stays a named constant instead of a bare 0x50 buried in the asm string.
//
// No register-clobber list is needed beyond memory: the kernel's stub saves and
// restores every GPR from the frame, so on return only RAX has changed. The
// "memory" clobber keeps the compiler from reordering the string's stores after
// the trap, so the kernel sees the finished string.
//
// always_inline is NOT decoration here, it is load-bearing. This kernel builds
// at -O0, where a plain `static inline` is emitted OUT OF LINE into .text at 1MB
// (kernel pages, not PG_USER). user_program lives in .user_text at 4MB; a ring-3
// call into a 1MB helper faults instantly (#PF, user fetch of a supervisor
// page). Forcing the inline folds the `int 0x50` directly into user_program, so
// every instruction the program runs stays inside its own user pages.
static inline __attribute__((always_inline)) uint64_t syscall1(uint64_t number, uint64_t arg1) {
    uint64_t ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// SYS_WRITE: hand the kernel a pointer to a NUL-terminated string to print.
// Returns 0 on success, (uint64_t)-1 if the kernel rejected the pointer.
static inline __attribute__((always_inline)) uint64_t sys_write(const char *string) {
    return syscall1(SYS_WRITE, (uint64_t)string);
}

// SYS_EXIT: ask the kernel to halt. It never returns, so neither do we; the
// infinite loop is only there to satisfy the compiler that control does not
// fall off the end back into ring 3.
static inline __attribute__((always_inline)) void sys_exit(void) {
    syscall1(SYS_EXIT, 0);
    for (;;) {
    }
}

// The strings live in .user_rodata, NOT the default .rodata. A plain string
// literal would land in .rodata at ~1MB, inside the kernel's pages, which are
// not PG_USER: the kernel could read it, but the pointer would point into
// memory this ring-3 program is not allowed to touch, and the syscall layer's
// bounds check (USER_REGION 4-8M) would (correctly) reject it. Forcing the
// bytes into .user_rodata, which the linker places in the 4-8M user region,
// puts them where a user pointer is supposed to point. `static const char[]`
// (not `const char *`) makes the section attribute apply to the CHARACTERS.
__attribute__((section(".user_rodata")))
static const char msg_hello[] = "user: hello from ring 3, via int 0x50\n";

__attribute__((section(".user_rodata")))
static const char msg_again[] = "user: and a second syscall, still in ring 3\n";

__attribute__((section(".user_text")))
void user_program(void) {
    sys_write(msg_hello);   // (1) cross into the kernel and print
    sys_write(msg_again);   // (2) do it again with a different string
    sys_exit();             // (3) ask the kernel to halt; never returns
}
