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

// The zero-, two-, and three-argument forms of the same doorbell. Same ABI: RAX =
// number, then RDI, RSI, RDX in System V order ("D" = RDI, "S" = RSI, "d" = RDX),
// return in RAX. Split out by arity rather than one variadic helper so each pins
// exactly the registers the kernel reads and no others.
static inline __attribute__((always_inline))
unsigned long syscall0(unsigned long number) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

static inline __attribute__((always_inline))
unsigned long syscall2(unsigned long number, unsigned long arg1, unsigned long arg2) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

static inline __attribute__((always_inline))
unsigned long syscall3(unsigned long number, unsigned long arg1,
                       unsigned long arg2, unsigned long arg3) {
    unsigned long ret;
    __asm__ __volatile__(
        "int %[vec]"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), [vec] "i"(SYSCALL_VECTOR)
        : "memory");
    return ret;
}

// SYS_WRITE: hand the kernel a pointer to a NUL-terminated string to print.
// Returns 0 on success, (unsigned long)-1 if the kernel rejected the pointer.
static inline __attribute__((always_inline))
unsigned long sys_write(const char *string) {
    return syscall1(SYS_WRITE, (unsigned long)string);
}

// SYS_EXIT: end THIS PROGRAM with `status` (masked by the kernel to 0..255). It no
// longer halts the machine, which is what it used to mean when there was no parent
// to return to: the task leaves the scheduler for good, its memory goes back to the
// kernel's pools, and a parent blocked in sys_wait is woken with this status.
//
// Never returns, and the infinite loop below is now GENUINELY unreachable rather
// than a formality for the compiler: the kernel never schedules this task again, so
// control does not come back to the instruction after the trap.
//
// A program must call this itself at the bottom of _start. There is no crt0 and
// nothing wraps the entry point, so a program that simply falls off the end of
// _start runs into whatever bytes follow it.
static inline __attribute__((always_inline))
void sys_exit(int status) {
    syscall1(SYS_EXIT, (unsigned long)(long)status);
    for (;;) {
    }
}

// SYS_WAIT: block until any child of this program exits, and return its exit status
// (0..255). Returns (unsigned long)-1 if this program has no children at all.
//
// ANY-CHILD, NOT waitpid: it takes no argument, so a program with several children
// is told about whichever finished first and cannot ask about a particular one.
// BLOCKING, like sys_readkey: waiting costs no CPU, and from here it looks like one
// call that took a while to come back.
static inline __attribute__((always_inline))
long sys_wait(void) {
    return (long)syscall0(SYS_WAIT);
}

// SYS_READKEY: pop one buffered keystroke, waiting for one if none is ready.
// BLOCKING: if the buffer is empty the kernel parks this task until a key arrives,
// so the call costs no CPU while it waits and there is no reason to poll it. The
// wait is invisible from here: it looks like one call that took a while. Always
// returns a real character (1..255).
static inline __attribute__((always_inline))
unsigned long sys_readkey(void) {
    return syscall0(SYS_READKEY);
}

// SYS_LIST: fill buf with the root directory's file names, one per line, NUL-
// terminated. Returns the number of names, or (unsigned long)-1 on error.
static inline __attribute__((always_inline))
unsigned long sys_list(char *buf, unsigned long size) {
    return syscall2(SYS_LIST, (unsigned long)buf, size);
}

// SYS_RUN: load and start the named program; it joins the scheduler alongside this
// one. Returns 0 on success, (unsigned long)-1 if it could not be started.
static inline __attribute__((always_inline))
unsigned long sys_run(const char *name) {
    return syscall1(SYS_RUN, (unsigned long)name);
}

// SYS_READFILE: read the named file into buf. Returns the number of bytes read, or
// (unsigned long)-1 on error. The bytes are RAW and NOT NUL-terminated: terminate
// them yourself before printing the buffer as a string.
static inline __attribute__((always_inline))
unsigned long sys_readfile(const char *name, char *buf, unsigned long size) {
    return syscall3(SYS_READFILE, (unsigned long)name, (unsigned long)buf, size);
}

// SYS_WRITEFILE: write `len` bytes from buf to the named file, creating it or
// wholly replacing it. The bytes are raw: no terminator is written and none is
// expected in buf. Returns 0 on success, (unsigned long)-1 on error (a bad
// pointer, a name that is not 8.3, no free space, or a disk error).
static inline __attribute__((always_inline))
unsigned long sys_writefile(const char *name, const char *buf, unsigned long len) {
    return syscall3(SYS_WRITEFILE, (unsigned long)name, (unsigned long)buf, len);
}

// SYS_DELETE: delete the named file from the disk. Returns 0 on success,
// (unsigned long)-1 on error (a bad pointer, a name that is not 8.3, a missing
// file, or a disk error).
static inline __attribute__((always_inline))
unsigned long sys_delete(const char *name) {
    return syscall1(SYS_DELETE, (unsigned long)name);
}

// SYS_FREECOUNT: how many clusters on the volume are free. Returns the count
// (never an error: a number crosses the boundary by value).
static inline __attribute__((always_inline))
unsigned long sys_freecount(void) {
    return syscall0(SYS_FREECOUNT);
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

// ============================================================================
// next_token: a reentrant, in-place tokenizer.
// ============================================================================
// It lives here, in the user-side runtime, rather than in libc/string.c on
// purpose. The user build compiles a single freestanding translation unit and
// links no kernel objects (see the user/%.ELF rule in the Makefile), so
// libc/string.c is simply not reachable from a ring-3 program; and the kernel
// itself never tokenizes anything, so putting it there would be dead code. So it
// sits beside the syscall wrappers, the rest of the runtime the shell is allowed
// to lean on, as a standalone function the program compiles directly.
//
// It is the strtok_r style: the caller holds the current position in *pos, so
// there is NO hidden global. That is deliberate, avoiding strtok's single static
// cursor, which makes strtok non-reentrant and is a classic action-at-a-distance
// bug (a second tokenization, even in a called function, clobbers the first).
//
// IN PLACE, NO COPYING: the separator after a token is overwritten with '\0' and
// the returned pointer points INTO the caller's buffer, so THE INPUT BUFFER IS
// MODIFIED. That is what lets the shell compare a token with a plain string
// compare and still reach the untouched remainder of the line after it.
static inline char *next_token(char **pos, char sep) {
    char *p = *pos;

    // SKIP LEADING SEPARATORS. A run of separators before the token is stepped
    // over, so "run   A.ELF" (extra spaces) yields "run" then "A.ELF" and never an
    // empty token in between. This is the non-obvious part: it is also why an empty
    // or all-separator remainder returns NULL (no more tokens) instead of a
    // zero-length token.
    while (*p == sep) {
        p++;
    }

    if (*p == '\0') {
        *pos = p;
        return (char *)0;   // no more tokens (NULL is not defined in this freestanding runtime)
    }

    // The token runs from here to the next separator or the end of the string.
    char *start = p;
    while (*p != '\0' && *p != sep) {
        p++;
    }

    if (*p == sep) {
        *p = '\0';   // shred: terminate this token in place
        p++;         // step past the separator so the next call resumes after it
    }
    // else *p is already '\0' (end of string), and *pos will point at it, so the
    // next call returns NULL.

    *pos = p;
    return start;
}

#endif
