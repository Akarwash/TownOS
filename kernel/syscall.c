#include "syscall.h"
#include "memory.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../include/syscalls.h"

// The value returned to ring 3 on any rejected or unknown request. Written into
// the saved frame's RAX, so iretq delivers it as the caller's return value.
#define SYSCALL_ERROR  ((uint64_t)-1)

// SYS_WRITE: print a NUL-terminated string supplied by the ring-3 caller.
//
// The pointer in RDI is UNTRUSTED. A ring-3 program could pass a kernel address
// and turn the kernel into a confused deputy, printing memory it is not allowed
// to read. Proper validation means checking the pointer's whole range against
// the caller's own mapped pages, which needs per-process address-space tracking
// that does not exist yet.
//
// STOPGAP, NOT REAL VALIDATION: we only check that the start pointer falls inside
// the single ring-3 region (USER_REGION_START..USER_REGION_END). This does NOT
// bound the string's length, so a string that starts just below USER_REGION_END
// with no NUL still walks out of the region and into kernel pages. See the TODO
// below and docs/reference/syscalls.md.
static uint64_t sys_write(uint64_t user_ptr) {
    // TODO: replace with a real per-process bounds check once address spaces
    // exist: validate the entire [ptr, ptr+len) range lies in the caller's pages,
    // and cap the length so an unterminated string cannot run off the region.
    if (user_ptr < USER_REGION_START || user_ptr >= USER_REGION_END) {
        print_string("syscall: SYS_WRITE rejected an out-of-bounds pointer\n");
        return SYSCALL_ERROR;
    }

    print_string((char *)user_ptr);
    return 0;
}

// SYS_READKEY: pop one character from the keyboard ring buffer, or 0 if none is
// waiting (drivers/keyboard.c). No pointer crosses the ring boundary here, so
// there is nothing to bounds-check: the character is returned by value in RAX.
//
// NON-BLOCKING BY DESIGN. This kernel has no way to sleep a task, so a blocking
// read (park the caller until a key arrives, wake it from the keyboard IRQ) cannot
// be built yet. Returning 0 on an empty buffer instead means the shell must
// busy-wait, calling this in a tight loop until it returns non-zero, burning a
// timeslice it could have yielded. That is the deliberate tradeoff; a real OS
// would block the task. TODO(blocking-readkey): block the caller once tasks can
// sleep, so the shell stops spinning on an empty buffer.
static uint64_t sys_readkey(void) {
    return (uint64_t)keyboard_getchar();
}

// SYS_EXIT: there is no scheduler and no parent to return to, so "exit" can only
// mean stop the machine. Halt with interrupts disabled, the same terminal state
// the exception handlers use.
static void sys_exit(void) {
    print_string("syscall: SYS_EXIT, halting.\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

// Dispatch on the call number in RAX. Unknown numbers are reported and rejected
// with SYSCALL_ERROR; a bad request must never fault or halt the kernel.
void syscall_handler(registers_t *regs) {
    switch (regs->rax) {
        case SYS_EXIT:
            sys_exit();                     // does not return
            break;
        case SYS_WRITE:
            regs->rax = sys_write(regs->rdi);
            break;
        case SYS_READKEY:
            regs->rax = sys_readkey();
            break;
        default:
            print_string("syscall: unknown number ");
            print_int((uint32_t)regs->rax);
            print_string(", rejected\n");
            regs->rax = SYSCALL_ERROR;
            break;
    }
}
