#include "file.h"
#include "heap.h"
#include "scheduler.h"          // task_block, WAIT_KEY (console read parks like sys_readkey)
#include "../drivers/screen.h"
#include "../drivers/keyboard.h" // keyboard_getchar, the console read source

// ============================================================================
// File descriptors: the read/write interface behind a task's fd table.
// ============================================================================
// A descriptor hides whether bytes are going to the screen or into a pipe. Stage 1
// knows only the console; stage 3 adds the pipe branch to file_write/file_read and
// the pipe-end bookkeeping to close_fd. See docs/decisions/0022-file-descriptors-
// and-pipes.md.

file_t *file_alloc_console(int writable) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (f == NULL) {
        return NULL;
    }
    f->kind = FD_CONSOLE;
    f->pipe = NULL;
    f->writable = writable;
    return f;
}

long file_write(file_t *f, const char *buf, uint32_t len, registers_t *r) {
    // The console is always ready, never blocks, and accepts everything it is
    // handed, so `r` (the block path's pile) is unused here.
    (void)r;

    if (f->kind == FD_CONSOLE) {
        // Print the counted buffer BYTE BY BYTE, not as a string. It is a counted
        // buffer that may contain no terminator and may legitimately contain zero
        // bytes, so print_string (which stops at the first NUL) would be wrong.
        for (uint32_t i = 0; i < len; i++) {
            print_char(buf[i]);
        }
        return (long)len;
    }

    // FD_PIPE is unreachable until stage 3 wires pipe_write in here: no pipe exists
    // to hold a descriptor yet.
    return FILE_ERR;
}

long file_read(file_t *f, char *buf, uint32_t len, registers_t *r) {
    if (f->kind == FD_CONSOLE) {
        // Drain up to `len` characters from the keyboard ring. If none is waiting,
        // park the task on WAIT_KEY exactly as sys_readkey does, and the keyboard IRQ
        // wakes it when a key arrives. A CONSOLE HAS NO END OF FILE: a keyboard is
        // never "done", so this never returns 0. Signalling console EOF would need a
        // Ctrl-D-style key and line discipline the driver does not have (ADR 0022).
        uint32_t n = 0;
        while (n < len) {
            int c = keyboard_getchar();
            if (c == 0) {
                break;
            }
            buf[n++] = (char)c;
        }
        if (n > 0) {
            return (long)n;
        }
        task_block(r, WAIT_KEY);
        return FILE_BLOCKED;      // parked; the caller must not touch rax
    }

    // FD_PIPE is unreachable until stage 3 wires pipe_read in here.
    return FILE_ERR;
}

void close_fd(file_t **fds, int fd) {
    file_t *f = fds[fd];
    if (f == NULL) {
        return;                  // already closed, or never opened: nothing to do
    }
    fds[fd] = NULL;

    // Stage 3 inserts the pipe end-count handling HERE, before the file_t is freed:
    // drop this end from the pipe's reader/writer count, wake a peer blocked on the
    // opposite condition when a count hits zero (the close IS the event that unblocks
    // an EOF-waiting reader, B2), and free the pipe_t once both counts are zero.
    kfree(f);
}
