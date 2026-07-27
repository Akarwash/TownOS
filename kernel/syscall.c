#include "syscall.h"
#include "memory.h"
#include "scheduler.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../fs/fat32.h"
#include "../include/syscalls.h"

// The longest filename SYS_RUN and SYS_READFILE will copy in from ring 3. An 8.3
// name needs at most 12 characters plus a terminator; 16 leaves a little slack.
// The cap is what stops an unterminated user string from walking off the region.
#define SYSCALL_NAME_MAX 16

// Is the whole range [ptr, ptr + len) inside the single ring-3 region? This is the
// security boundary for every syscall that is handed a buffer to write into or a
// string to read out of. The pointer comes from ring 3 and is UNTRUSTED, so it is
// checked BEFORE the kernel touches a byte through it (same spirit as the ELF
// loader's segment bounds check in kernel/elf.c). Unlike the SYS_WRITE stopgap
// below, this bounds the ENTIRE range, not just the start, and it is careful about
// overflow: ptr + len can wrap on a crafted length, and a wrapped sum compares as
// comfortably small, so len is checked against the room above ptr rather than by
// forming ptr + len.
static int user_range_ok(uint64_t ptr, uint64_t len) {
    if (ptr < USER_REGION_START || ptr >= USER_REGION_END) {
        return 0;
    }
    return len <= USER_REGION_END - ptr;
}

// Copy a NUL-terminated string from ring 3 into a kernel buffer, capping the
// length so a missing terminator cannot walk out of the region. The start pointer
// is bounds-checked; then bytes are copied until a NUL, until dst_size is reached,
// or until USER_REGION_END is reached, whichever comes first. The result is always
// NUL-terminated. Returns 0 on success, -1 if the start pointer is out of bounds or
// no terminator appears within the cap.
static int copy_user_string(uint64_t user_ptr, char *dst, uint32_t dst_size) {
    if (dst_size == 0) {
        return -1;
    }
    if (user_ptr < USER_REGION_START || user_ptr >= USER_REGION_END) {
        return -1;
    }
    for (uint32_t i = 0; i < dst_size; i++) {
        uint64_t addr = user_ptr + i;
        if (addr >= USER_REGION_END) {
            return -1;   // reached the region edge with no terminator
        }
        char c = *(const char *)addr;
        dst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    dst[dst_size - 1] = '\0';   // cap hit: truncate rather than overflow
    return -1;                  // the name did not fit, treat as invalid
}

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

// SYS_READKEY: pop one character from the keyboard ring buffer, sleeping until one
// arrives if the buffer is empty. No pointer crosses the ring boundary here, so
// there is nothing to bounds-check: the character is returned by value in RAX.
//
// BLOCKING. On an empty buffer the caller is parked (TASK_BLOCKED, WAIT_KEY) and
// the CPU goes to another task, or idles if there is none. The keyboard IRQ wakes
// it once it has a key. The caller cannot tell: from ring 3 this is one `int 0x50`
// that took a long time to come back.
//
// THERE IS NO LOOP HERE, and that is the point. Re-checking the buffer in a loop
// inside this handler would just move the shell's old busy-wait into ring 0, where
// it is worse: the kernel would spin with the scheduler unable to run and every
// other task starved. Instead this function is entered, finds nothing, blocks, and
// ENDS. What resumes the task is a fresh entry into this same handler later.
//
// Unlike this call's own arguments, the "did a key arrive" question is answered by
// the wake itself: a WAIT_KEY task is only ever woken by a push into the buffer, so
// the retry finds a key. If a wake ever proved spurious, the retry would simply
// block again, which is one more clean yield rather than a spin.
//
// Writing to regs->rax is done HERE rather than by the dispatcher, and only on the
// path that actually has an answer. On the blocking path rax must be left holding
// SYS_READKEY, because task_block rewinds rip onto the `int 0x50` and the CPU takes
// the syscall number from rax when that instruction runs again. Overwriting it with
// a return value would make the woken task issue a DIFFERENT call: rax = 0 is
// SYS_EXIT, so a stray "return 0" here would halt the machine on the next keypress.
static void sys_readkey(registers_t *regs) {
    int c = keyboard_getchar();
    if (c != 0) {
        regs->rax = (uint64_t)c;
        return;
    }

    task_block(regs, WAIT_KEY);

    // Unreachable on the blocking path: task_block redirected the pile through
    // schedule(), so this kernel entry now belongs to another task and ends at its
    // iretq. Nothing may be added after this call, least of all a write to rax.
}

// SYS_LIST: write the root directory's file names into a ring-3 buffer, one per
// line and NUL-terminated, and return how many were written.
//   RDI = user buffer pointer, RSI = buffer size.
// The pointer is UNTRUSTED, so the WHOLE [buf, buf+size) range is bounds-checked
// against the ring-3 region before the kernel writes a single byte through it.
// Returns the number of names written, or -1 on a bad pointer or a filesystem
// error. fat32_list_names silently drops names that do not fit.
static uint64_t sys_list(uint64_t user_buf, uint64_t bufsize) {
    if (!user_range_ok(user_buf, bufsize)) {
        print_string("syscall: SYS_LIST rejected an out-of-bounds buffer\n");
        return SYSCALL_ERROR;
    }
    uint32_t count = 0;
    if (fat32_list_names((char *)user_buf, (uint32_t)bufsize, &count) != 0) {
        return SYSCALL_ERROR;
    }
    return count;
}

// SYS_RUN: load and start the program named by a ring-3 string, joining it to the
// scheduler alongside the caller.
//   RDI = user pointer to a NUL-terminated 8.3 filename.
// The name is copied into the kernel first (bounds-checked and length-capped, so a
// missing terminator cannot run off the region). task_create_from_file does the
// rest, and it already REPORTS AND SKIPS a missing or malformed program rather than
// faulting, so a bad name here costs nothing but a returned -1: the kernel is never
// taken down by what a program asks to run. Returns 0 on success, -1 on a bad
// pointer or a load failure.
static uint64_t sys_run(uint64_t user_name) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_RUN rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    // The caller becomes the new task's parent, which is what later lets it wait
    // for the program it started. The id has to be taken HERE, inside the syscall,
    // because `current` is only the requesting task while its own syscall is being
    // served; by the next timer tick it means somebody else.
    if (task_create_from_file(name, scheduler_current_id()) < 0) {
        print_string("syscall: SYS_RUN could not start ");
        print_string(name);
        print_string("\n");
        return SYSCALL_ERROR;
    }
    return 0;
}

// SYS_READFILE: read a whole file off the disk into a ring-3 buffer.
//   RDI = user pointer to a NUL-terminated 8.3 filename,
//   RSI = user buffer pointer, RDX = buffer size.
// BOTH pointers are untrusted and BOTH are checked before use: the filename is
// copied in (capped) and the destination [buf, buf+size) is confirmed to lie in the
// ring-3 region. fat32_read_file writes at most the file's own size and refuses
// outright if it exceeds the buffer, so nothing overruns. Returns the number of
// bytes read, or -1 on a bad pointer, a missing file, or a read error. The bytes
// are raw file contents and are NOT NUL-terminated; the caller terminates them
// before treating the buffer as a string to print.
static uint64_t sys_readfile(uint64_t user_name, uint64_t user_buf, uint64_t bufsize) {
    char name[SYSCALL_NAME_MAX];
    if (copy_user_string(user_name, name, sizeof(name)) != 0) {
        print_string("syscall: SYS_READFILE rejected a bad filename pointer\n");
        return SYSCALL_ERROR;
    }
    if (!user_range_ok(user_buf, bufsize)) {
        print_string("syscall: SYS_READFILE rejected an out-of-bounds buffer\n");
        return SYSCALL_ERROR;
    }
    uint32_t read_size = 0;
    if (fat32_read_file(name, (void *)user_buf, (uint32_t)bufsize, &read_size) != 0) {
        return SYSCALL_ERROR;
    }
    return read_size;
}

// SYS_EXIT: end the calling task with the status in RDI.
//   RDI = exit status, masked to 0..255 by task_exit.
// This used to halt the machine, because there was no way for a task to stop
// without stopping everything: no parent to return to and no way to reclaim what
// the task held. Now it is a real exit. The task is marked TASK_ZOMBIE and leaves
// the rotation permanently, its parent is woken if it was waiting, and its memory
// is returned to the pools later by the scheduler's sweeper, from a tick that is no
// longer standing on the dying task's page tables.
//
// Does not return: task_exit ends in schedule(), which redirects the register pile
// into a different task, so this kernel entry finishes at that task's iretq.
static void sys_exit(registers_t *regs) {
    task_exit(regs, (int)regs->rdi);
}

// SYS_WAIT: block until any child of the caller exits, and return its exit status.
// No arguments: this is any-child, not waitpid, so a parent with several children
// gets whichever finished first and cannot ask for a particular one. Returns that
// child's status (0..255), or SYSCALL_ERROR if the caller has no children at all,
// which is the one case that cannot be answered by waiting.
//
// BLOCKING, and therefore subject to the same rule as SYS_READKEY: on the waiting
// path task_wait leaves RAX alone so the re-armed `int 0x50` still reads SYS_WAIT
// out of it. RAX is written only on the two paths that have an answer. See the long
// comment on sys_readkey above, and task_wait in kernel/scheduler.c.
static void sys_wait(registers_t *regs) {
    task_wait(regs);
}

// Dispatch on the call number in RAX. Unknown numbers are reported and rejected
// with SYSCALL_ERROR; a bad request must never fault or halt the kernel.
void syscall_handler(registers_t *regs) {
    switch (regs->rax) {
        case SYS_EXIT:
            sys_exit(regs);   // does not return; deliberately not `regs->rax = ...`
            break;
        case SYS_WRITE:
            regs->rax = sys_write(regs->rdi);
            break;
        case SYS_READKEY:
            sys_readkey(regs);   // writes rax itself, and must not on the block path
            break;
        case SYS_LIST:
            regs->rax = sys_list(regs->rdi, regs->rsi);
            break;
        case SYS_RUN:
            regs->rax = sys_run(regs->rdi);
            break;
        case SYS_READFILE:
            regs->rax = sys_readfile(regs->rdi, regs->rsi, regs->rdx);
            break;
        case SYS_WAIT:
            sys_wait(regs);   // writes rax itself, and must not on the block path
            break;
        default:
            print_string("syscall: unknown number ");
            print_int((uint32_t)regs->rax);
            print_string(", rejected\n");
            regs->rax = SYSCALL_ERROR;
            break;
    }
}
