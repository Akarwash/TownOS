#include "file.h"
#include "heap.h"
#include "../drivers/screen.h"

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
