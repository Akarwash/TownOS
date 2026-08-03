# File descriptors

A **descriptor** is a small integer that names an open destination for bytes. A
program writes "fd 1" or reads "fd 0" without knowing whether that is the screen, the
keyboard, or a pipe into another program: the descriptor hides the difference. This
page documents the per-task descriptor table, the two kinds of entry, and the calls
that use them. Read from `kernel/file.h`, `kernel/file.c`, `kernel/syscall.c`, and
`kernel/scheduler.h`. For the rationale, see
[decision 0022](../decisions/0022-file-descriptors-and-pipes.md); for pipes
specifically, [pipes.md](pipes.md).

## The table

Each `task_t` (`kernel/scheduler.h`) carries a fixed array `fds[MAX_FDS]` with
`MAX_FDS` = 8, `NULL` where a slot is unused. Each non-NULL slot points at a `file_t`:

```c
typedef enum { FD_CONSOLE, FD_PIPE } fd_kind_t;

typedef struct file {
    fd_kind_t     kind;
    struct pipe  *pipe;      // NULL unless kind == FD_PIPE
    int           writable;  // 1 = an output/write end, 0 = an input/read end
} file_t;
```

A `file_t` is **owned by exactly one slot in one task** and is never shared between
slots. There is no reference count on it, deliberately: the only shared object is the
`pipe_t` a pipe end points at, and its `readers`/`writers` counts are the single place
a pipe end is tallied (see [pipes.md](pipes.md) and B4 in
[decision 0022](../decisions/0022-file-descriptors-and-pipes.md)).

## The 0-is-input, 1-is-output convention

Every task is born with a console on **fd 0** (an input end that reads the keyboard)
and **fd 1** (an output end that writes the screen), set up in `task_register`. That 0
is standard input and 1 is standard output is a **convention** shared by the kernel,
every program, and the shell — nothing in the hardware or the dispatcher enforces it.
The `writable` flag is what the kernel does enforce: `SYS_WRITE` rejects a fd that is
not writable (fd 0, or a pipe's read end), and `SYS_READ` rejects one that is (fd 1,
or a pipe's write end).

## The calls

They follow the calling convention in [syscalls.md](syscalls.md): the number in RAX,
arguments in RDI/RSI/RDX, result in RAX.

| Number | Name | Arguments | Returns |
|--------|------|-----------|---------|
| 1 | `SYS_WRITE` | RDI = fd, RSI = buffer, RDX = length | bytes written (may be < length), or -1 |
| 11 | `SYS_READ` | RDI = fd, RSI = buffer, RDX = length | bytes read, 0 at EOF, or -1 |
| 12 | `SYS_CLOSE` | RDI = fd | 0, or -1 on a bad fd |
| 13 | `SYS_PIPE` | RDI = `int[2]` out | 0 (writes `[read_fd, write_fd]`), or -1 |

- **A count may be short.** `SYS_WRITE` and `SYS_READ` move at most `SYSCALL_IO_MAX`
  (4096) bytes per call — the size of the kernel staging buffer the counted user
  buffer is copied through — and a pipe moves only what fits or is available. A caller
  never assumes one call moved everything; it loops on the count (see B5 in
  [decision 0022](../decisions/0022-file-descriptors-and-pipes.md)). `sys_print` in
  `userlib.h` is that loop for the common "print a string to fd 1" case.
- **`SYS_READ` and `SYS_WRITE` can block, and carry the RAX discipline.** A read on an
  empty pipe (with a live writer) or an empty console, and a write to a full pipe
  (with a live reader), park the task; the handler writes nothing to RAX on that path,
  because the re-armed `int 0x50` reads the syscall number back out of it. See
  [blocking.md](blocking.md).
- **`SYS_CLOSE`** frees the `file_t` and clears the slot. For a pipe end it also drops
  the pipe's end-count, wakes a blocked peer when a count reaches zero, and frees the
  `pipe_t` when both do ([pipes.md](pipes.md)).
- **`SYS_PIPE`** allocates one `pipe_t` and two `file_t` into two of the caller's
  slots, and writes the two descriptor numbers through the `int[2]` out pointer, which
  is bounds-checked as a write target first. The numbers are chosen by the kernel
  (`alloc_fd` picks the lowest free slot) and index the caller's own table only.

## How a child gets a descriptor

Nothing can inject a descriptor into a running task; a task's table is set at creation
and only it changes it afterwards. So the only way a child acquires anything other
than its default console is **at `SYS_RUN`**: `sys_run(name, in_fd, out_fd)` gives the
child those two descriptors of the caller's as its fd 0 and fd 1 (or -1 for a fresh
console). `task_create_from_file` validates them (in_fd must be a read end, out_fd a
write end), duplicates each into a new `file_t` of the child's own pointing at the
same `pipe_t` (`file_dup`, which counts the new end), and swaps them in for the
default console. That shared `pipe_t` pointer is the entire connection between two
tasks in a pipeline: one buffer, two tables, different indices.

`task_exit` closes every descriptor a task still holds, which is what makes a pipe
writer's exit deliver EOF to the reader downstream.

## Related

- Pipes, the second kind of descriptor: [pipes.md](pipes.md),
  [decision 0022](../decisions/0022-file-descriptors-and-pipes.md).
- The shell that wires pipelines out of these calls: [shell.md](shell.md).
- The blocking the read/write paths use: [blocking.md](blocking.md).
- The syscall gate and convention: [syscalls.md](syscalls.md).
