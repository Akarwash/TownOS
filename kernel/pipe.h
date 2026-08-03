#ifndef PIPE_H
#define PIPE_H

#include "isr.h"               // registers_t, for the block path
#include "../include/types.h"

// ============================================================================
// A pipe: a byte stream between a writer end and a reader end.
// ============================================================================
// SAME SHAPE AS THE KEYBOARD RING BUFFER (drivers/keyboard.c), deliberately, and it
// should stay recognisably so: a fixed circular buffer where write_index chases
// read_index and ONE SLOT IS ALWAYS LEFT UNUSED, so that write_index == read_index
// means empty and can never also mean full. What a pipe has that the keyboard ring
// does not is the two COUNTS: an end can be closed, and both EOF ("the last writer
// went away") and a broken pipe ("the last reader went away") are defined by a count
// reaching zero. See docs/reference/pipes.md and
// docs/decisions/0022-file-descriptors-and-pipes.md.
#define PIPE_SIZE 4096

typedef struct pipe {
    char     buf[PIPE_SIZE];
    uint32_t read_index;    // next slot to drain
    uint32_t write_index;   // next slot to fill
    int      readers;       // file_t ends open for reading; 0 => writes are broken
    int      writers;       // file_t ends open for writing; 0 => reads see EOF
} pipe_t;

// Allocate an empty pipe with NO ends yet (readers = writers = 0). Each end is
// counted as it is created (file_alloc_pipe) or inherited (file_dup), and uncounted
// as it is closed (file_close), so the counts always equal the number of live ends.
// Returns NULL if the heap is out of memory.
pipe_t *pipe_create(void);

// Move up to `len` bytes out of / into the pipe. Return the count moved (0 from a
// read is EOF), FILE_BLOCKED if the call parked the task, or FILE_ERR. `r` is the
// live syscall pile, used only on the block path. The wake/block/EOF rules live in
// pipe.c; the short version is: writing wakes a blocked reader, reading wakes a
// blocked writer, an empty pipe with a live writer blocks the reader, a full pipe
// with a live reader blocks the writer, an empty pipe with no writer is EOF, and a
// write to a pipe with no reader is a broken-pipe error.
long pipe_read(pipe_t *p, char *buf, uint32_t len, registers_t *r);
long pipe_write(pipe_t *p, const char *buf, uint32_t len, registers_t *r);

#endif
