#ifndef FILE_H
#define FILE_H

#include "isr.h"               // registers_t, for the block-capable I/O calls
#include "../include/types.h"

// ============================================================================
// A file descriptor: one open destination in one task's table.
// ============================================================================
// A task holds a small fixed table of these (task_t.fds, see scheduler.h). A
// descriptor names WHERE bytes go or come from, hiding the difference between the
// console and a pipe behind one read/write interface. This is the machinery that
// lets a program write "output" without caring whether output is the screen or the
// far end of a pipe into another program. See docs/reference/descriptors.md and
// docs/decisions/0022-file-descriptors-and-pipes.md.

// A pipe is the other kind of destination; a file_t only ever holds a POINTER to
// one, so a forward declaration is all this header needs. The full definition is in
// kernel/pipe.h and is pulled in only by the .c files that touch a pipe's innards.
struct pipe;

// What a descriptor points at. Two kinds today: the console (the screen for
// writing, the keyboard for reading) and a pipe. A file redirect (a real disk file
// behind an fd) would be a third and is deliberately absent — see ADR 0022.
typedef enum { FD_CONSOLE, FD_PIPE } fd_kind_t;

// One open destination.
//
// OWNED BY EXACTLY ONE TABLE SLOT IN EXACTLY ONE TASK, never shared between slots.
// THERE IS NO REFERENCE COUNT, and adding one would be a bug rather than a tidy-up.
// When a child inherits a pipe end (task_create_from_file) it gets a FRESHLY
// ALLOCATED file_t of its own that merely points at the same `struct pipe`. The one
// shared object in the whole scheme is that pipe_t, whose readers/writers counts are
// the single place a pipe end is counted. A refcount here would duplicate those two
// counts, the two could drift apart, and every close would have to keep them in
// agreement — which is exactly the class of bug (B4 in ADR 0022) that one-owner-per-
// slot removes. Do not "fix" the fact that sys_run allocates a second file_t by
// sharing one: the second allocation is the design.
typedef struct file {
    fd_kind_t     kind;
    struct pipe  *pipe;      // NULL unless kind == FD_PIPE
    int           writable;  // 1 = an output/write end, 0 = an input/read end
} file_t;

// Sentinel returns from file_write/file_read (and the pipe ops in pipe.h), distinct
// from any real byte count (0..len, where 0 from a read is EOF) and from each other:
//   FILE_BLOCKED — the call parked the task with task_block; the syscall handler
//                  MUST return without writing rax, because the re-armed `int 0x50`
//                  reads the syscall number back out of rax (docs/reference/blocking.md).
//   FILE_ERR     — an I/O error (a bad fd is caught earlier, in the syscall layer).
#define FILE_BLOCKED  (-2L)
#define FILE_ERR      (-1L)

// Allocate a console descriptor. `writable` picks the direction: 1 is an output end
// (writes go to the screen), 0 is an input end (reads drain the keyboard). Returns
// NULL if the heap is out of memory.
file_t *file_alloc_console(int writable);

// Allocate a pipe descriptor pointing at `p`, and COUNT the new end on the pipe
// (writers++ if writable, else readers++). `writable` 1 is the write end, 0 the read
// end. Returns NULL on OOM, having counted nothing. The count is the number of live
// ends, which is why creating one bumps it and file_close drops it — there is no
// separate refcount on the file_t (B4).
file_t *file_alloc_pipe(struct pipe *p, int writable);

// Free a file_t not held by any table slot, dropping its pipe end-count. This is the
// core close_fd delegates to; it is exposed so a half-built inheritance in
// task_create_from_file can undo one dup cleanly. For a pipe end it decrements the
// matching count, WAKES a peer blocked on the opposite condition when that count
// reaches zero (a close is the event that gives a waiting reader EOF, B2), and frees
// the pipe_t once both counts are zero.
void file_close(file_t *f);

// Lowest free slot in the table `fds`, or -1 if it is full. The kernel chooses the
// number and hands it back; it indexes THIS task's table only.
int alloc_fd(file_t **fds);

// Write up to `len` bytes of `buf` to `f`, returning the count actually written
// (which MAY BE LESS than len — a pipe takes only what fits), FILE_BLOCKED if it
// parked the task, or FILE_ERR. `r` is the live syscall pile, needed only on the
// path that blocks (a full pipe with a live reader). A console never blocks and
// always accepts everything.
long file_write(file_t *f, const char *buf, uint32_t len, registers_t *r);

// Read up to `len` bytes from `f` into `buf`, returning the count read (0 means
// end of file: a pipe drained with no writer left), FILE_BLOCKED if it parked the
// task (an empty pipe with a live writer, or an empty console), or FILE_ERR. `r` is
// the live syscall pile for the block path. A console never returns EOF.
long file_read(file_t *f, char *buf, uint32_t len, registers_t *r);

// Close descriptor `fd` in the table `fds`: free the file_t and clear the slot. A
// no-op if the slot is already empty. For a pipe end (stage 3) this also drops the
// end-count and, when a count reaches zero, wakes a blocked peer and frees the pipe.
// Takes the fds array rather than a task_t so this header need not know that type.
void close_fd(file_t **fds, int fd);

#endif
