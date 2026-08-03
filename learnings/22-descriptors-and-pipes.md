# Chapter 22: File Descriptors and Pipes

> Read chapter 19 (blocking and sleep) and chapter 14 (per-process paging) first.
> This chapter is written code-first rather than in the metaphor voice used
> elsewhere in this folder. The metaphors for this subject (chutes, mailboxes,
> hoses) all leak, because the thing being described is a pointer stored in two
> tables, and every analogy for that ends up implying a delivery step that does not
> exist.

## Where we are

Here is the entire output path of a program today:

```c
// user/tests/A.c
sys_write("A");
```

```c
// kernel/syscall.c
case SYS_WRITE:
    regs->rax = sys_write(regs->rdi);
```

One argument, the string. The destination is not an argument. The handler calls
`print_string` and the bytes land on the VGA buffer. There is no other possible
outcome, and no way to write a program whose output goes anywhere else.

Which means two programs cannot hand each other anything. Chapter 14 made their
address spaces private on purpose, so they share no memory, and the only channel
between them is the disk, whole-file, at millisecond speeds.

## Rule 1: make the destination an argument

```c
sys_write(1, "A", 1);
```

That `1` is not the console. It is an index:

```c
typedef struct {
    registers_t regs;
    address_space_t *aspace;
    ...
    file_t *fds[MAX_FDS];    // <- new
} task_t;
```

And the handler stops naming a destination:

```c
static uint64_t sys_write(uint64_t fd, uint64_t user_buf, uint64_t len) {
    task_t *t = tasks[current];
    if (fd >= MAX_FDS || t->fds[fd] == NULL) return SYSCALL_ERROR;
    return file_write(t->fds[fd], buf, len);
}
```

```c
long file_write(file_t *f, const char *buf, uint32_t len) {
    if (f->kind == FD_CONSOLE) { print_string(buf, len); return len; }
    if (f->kind == FD_PIPE)    { return pipe_write(f->pipe, buf, len); }
    return SYSCALL_ERROR;
}
```

That `if` is the whole routing mechanism. A tag on a struct and a branch. Nothing
is discovered, computed, or negotiated at write time.

## Rule 2: the number is an index, not an address

```c
A's     fds[1] → &console_file
COUNT's fds[1] → &console_file
A's     fds[1] → &pipe_file    // if the shell wired it that way
```

The same number, `1`, in two different tables, meaning two different things. So the
number cannot be carrying destination information. It is an array index, and
`arr[1]` tells you nothing without also knowing `arr`.

The lookup is always `tasks[current]->fds[fd]`. **There is no argument that selects
a different table.** `current` is whoever trapped into the kernel. So a program
guessing numbers can only ever reach its own slots, and the ones it was not given
are NULL and rejected.

If that shape feels familiar, it is chapter 14 one level up. Address `0x400000`
means different physical memory depending on which CR3 is loaded. Descriptor 3
means a different destination depending on which task is asking. In both cases the
number is meaningless until you know whose table, and the kernel always knows
whose, because it is the one that got trapped into.

## Rule 3: the program cannot see its own table

Worth being exact about, because "the program does not know where its output goes"
sounds like a politeness rather than a fact.

`task_t` is on the kernel heap. Kernel pages have the user bit clear (chapter 10).
So a ring-3 program attempting to read `fds[]` takes a page fault. There is no
syscall to ask what a descriptor points at, and there could not usefully be one.

The operational test: could a program write this?

```c
if (output_is_a_pipe()) { ... } else { ... }
```

No. That function cannot be implemented from ring 3. That is what "does not know"
means here.

Compare `sys_writefile("NOTES.TXT", buf, len)`, where the destination is a string
in the program's own memory: readable, printable, branchable. With a descriptor the
destination is not in the program at all.

## Rule 4: 0 and 1 are a convention, nothing more

```c
// task_register
t->fds[0] = console_entry();   // input
t->fds[1] = console_entry();   // output
// fds[2..7] = NULL
```

Nothing in the hardware, and nothing in the dispatcher, says 1 means output.
`file_write` switches on `f->kind`, never on the index. Three parties simply agreed:

- the kernel fills 0 and 1 at task creation
- every program is written to use 0 and 1
- the shell overwrites them when it wants to redirect

Break any one and it stops working. That is why real systems give the numbers names,
`STDIN_FILENO` and `STDOUT_FILENO`: a label for a number everybody already agreed
to.

Keep two things apart that are easy to blur:

- **The slot number is the direction.** 0 in, 1 out. Never varies.
- **The slot contents are the device.** Console, pipe, later a file. This is what
  varies.

|  | fds[0] (input) | fds[1] (output) |
|---|---|---|
| normally | console → keyboard | console → screen |
| piped | pipe read end | pipe write end |

The console appears in both columns because the console is itself two things.

## Rule 5: a pipe is a buffer, and both ends store the same pointer

```c
typedef struct pipe {
    char     buf[PIPE_SIZE];
    uint32_t read_index;
    uint32_t write_index;
    int      readers;
    int      writers;
} pipe_t;
```

That is `kbd_buffer`, `kbd_read_index` and `kbd_write_index` from
`drivers/keyboard.c`, with two counts added. Same ring, same wasted slot so that
full and empty stay distinguishable. You built the hard part of a pipe three rungs
ago and called it a keyboard.

The connection is not a lookup. It is one `kmalloc` and two stored copies of the
result:

```c
pipe_t *p = kmalloc(sizeof(pipe_t));     // say 0x201000

A's     fds[1] = { FD_PIPE, .pipe = 0x201000, .writable = 1 };
COUNT's fds[0] = { FD_PIPE, .pipe = 0x201000, .writable = 0 };
```

A writes into the struct at 0x201000. COUNT reads out of the struct at 0x201000.
There is no delivery step, no routing table, and no point at which the kernel asks
which program is on the other end. **They are connected because both slots hold the
same address.**

And that address resolves identically from both tasks because the kernel is mapped
into every address space (chapter 14), by value, with the user bit clear. The
kernel heap is the only region where one address means the same bytes to every
task, which is why the buffer has to live there and cannot live in either program.

## Rule 6: reading consumes, and boundaries do not survive

Two properties that follow directly from there being two indices and a char array,
and no other fields.

**Reading destroys.** `read_index` moves past the byte and it is gone. Nobody else
can have it and the reader cannot have it twice. There is no seek, because the
bytes behind `read_index` are already being overwritten by writes wrapping around.
A file is a value you can read repeatedly. A pipe is a stream you can read once.

**Message boundaries vanish.**

```c
sys_write(1, "AAA", 3);
sys_write(1, "BBBBB", 5);
```

The buffer holds `AAABBBBB`. Nothing records that there were two calls, because
there is no field in `pipe_t` in which to record it. A reader asking for 4 gets
`AAAB`, splitting across what the writer thought were separate messages.

So a program that needs to know where one record ends must put a marker *in the
bytes*, a newline or a length prefix, and scan for it. That agreement lives entirely
in the two programs. The kernel keeps no record because it has nowhere to keep one.

There is an alternative design where the kernel does preserve boundaries and does
carry addresses. Those are message queues, and Unix has them too. Pipes are
deliberately the dumber primitive.

## Rule 7: the buffer being finite is the flow control

The keyboard's producer drops on full:

```c
static void kbd_buffer_push(char c) {
    uint32_t next = (kbd_write_index + 1) % KBD_BUFFER_SIZE;
    if (next == kbd_read_index) return;   // full: drop
    ...
}
```

Correct for a keypress, because a dropped key can be retyped. Wrong for program
output, because there is nothing to retype: those bytes are the result of work that
already happened, and dropping them gives the downstream program a hole in its input
with no indication anything went wrong.

So the writer blocks instead:

```c
if (no_space && p->readers > 0) {
    task_block(r, WAIT_PIPE_WRITE);
    return;                    // and DO NOT touch regs->rax
}
```

And symmetrically the reader blocks on empty, on `WAIT_PIPE_READ`. Each side wakes
the other: a write wakes readers, a read frees a slot and wakes writers. Chapter
19's rule, unchanged: whoever causes the event wakes the waiters.

That symmetry is the entire flow control mechanism. A fast writer throttles itself
to a slow reader with no negotiation, no protocol, and no messages exchanged. The
buffer being finite is the only thing coordinating them.

Note the wake is on **any** free slot, not on empty. Waiting for empty would force
strict alternation, one side working at a time. Waking on a single freed slot lets
the reader drain the front while the writer fills the back, which is the entire
point of a pipeline. The cost is that a pathological case can thrash, wake, write
one byte, block. Real kernels use a watermark. At 100 Hz with two tasks, yours does
not need one.

### And the RAX trap is live again

Chapter 19's sharpest edge applies to both new syscalls. `task_block` rewinds `rip`
onto the `int 0x50`, so when the woken task re-executes it, the CPU reads the
syscall number out of RAX again. Writing a return value there on the blocking path
makes the woken task issue a *different* syscall. Write 0 and it issues `SYS_EXIT`
and kills itself.

Which also means `pipe_read` and `pipe_write` must be safe to run twice, since the
woken task re-issues the whole call from the top. They are: a call that transfers
nothing changes nothing.

## Rule 8: empty is not finished

The last hard thing, and the one that has hung the most people.

A reader finds the pipe empty. Two completely different situations, identical from
inside the buffer:

- The writer is slow and will write later. Block.
- The writer has exited. Nothing will ever arrive. Blocking is forever.

The indices cannot tell them apart. So the pipe has to count who could still write:

```c
if (empty(p)) {
    if (p->writers == 0) return 0;      // EOF
    task_block(r, WAIT_PIPE_READ);
}
```

`sys_read` returning 0 means end of file. That is why `read()` returning 0 means EOF
in Unix rather than "nothing available right now".

### Why counts and not a boolean

Because the shell necessarily holds both ends for a while:

```
sys_pipe(p)                writers=1     shell has the write end
sys_run(a, -1, p[1])       writers=2     a got a copy
sys_close(p[1])            writers=1     shell dropped its copy
```

Two, during setup, in the simplest possible pipeline. A boolean cannot express that.

And this is where the classic bug lives. Drop that `sys_close` and `writers` sticks
at 1 forever. The downstream program blocks on an empty pipe waiting for a shell
that is never going to write, the shell blocks in `wait` for a child that will never
exit, and both hang with no error and no output.

### Closing is an event, so closing must wake

The subtle consequence. A reader already blocked on an empty pipe is woken by a
write. If the last writer *closes* rather than writing, nothing wakes it, and it
waits forever for an EOF it can never observe.

So `close_fd`, when it drops `writers` to zero, must `scheduler_wake(WAIT_PIPE_READ)`.
And when it drops `readers` to zero, `scheduler_wake(WAIT_PIPE_WRITE)`.

Chapter 19's rule again, applied to a case that does not look like an event: the
thing that causes the condition is the thing that wakes the waiters, and "the last
writer went away" is a condition somebody is waiting on.

### Destruction is a refcount, not a decision

```c
if (p->readers == 0 && p->writers == 0) kfree(p);
```

Nobody declares a pipe finished. It is destroyed when nothing refers to it, which is
the same shape as the reap sweeper in chapter 20: the resource goes away when the
last holder lets go.

## Rule 9: only the parent can wire

A running task cannot be given a descriptor. Nothing can reach into `task_t` and add
an entry, and the program cannot ask for something it does not know exists.

So the wiring is done at spawn:

```c
sys_run("A.ELF", -1, p1[1]);      // A's fds[1] = the write end
sys_run("COUNT.ELF", p1[0], -1);  // COUNT's fds[0] = the read end
```

Unix does this differently, with a gap. `fork` copies the current process, `exec`
replaces its contents, and in between the child is running and can rearrange its own
descriptors before the new program loads. All the plumbing happens in that gap.

`SYS_RUN` has no gap: it goes from nothing to a running ring-3 program in one call.
So the ends are passed in as arguments instead. Less general, and honest about what
this kernel is. `fork` remains available later, and copy-on-write lives there.

## Rule 10: what `|` actually is

```
run a.elf | run upper.elf | run count.elf
```

Three segments, so two pipes. The shell splits on the character, counts, and loops:

```c
int in_fd = -1;
for (int i = 0; i < n; i++) {
    int out_fd = -1, next_in = -1;
    if (i < n - 1) {
        int p[2];
        sys_pipe(p);
        out_fd  = p[1];
        next_in = p[0];
    }
    start(seg[i], in_fd, out_fd);
    if (in_fd  != -1) sys_close(in_fd);
    if (out_fd != -1) sys_close(out_fd);
    in_fd = next_in;
}
```

`in_fd = next_in` is the linking. Each iteration's read end becomes the next
iteration's input.

|  | fds[0] | fds[1] |
|---|---|---|
| a.elf | console | p1 write |
| upper.elf | p1 read | p2 write |
| count.elf | p2 read | console |

Only the first has the keyboard. Only the last has the screen. The middle has pipes
on both sides, and its code is identical to the others: `sys_read(0, ...)` and
`sys_write(1, ...)`.

And this is the payoff. `a.elf` was compiled before `count.elf` existed. Neither
contains any reference to the other. The knowledge of what connects to what exists
in exactly one place, the shell, for the duration of that loop, and nowhere else
afterwards. That is what makes it possible to snap together programs in an order
nobody anticipated, and it is why Unix bet on this primitive.

## Rule 11: streaming versus buffering is the program's business

`upper.elf` writes as it reads. `sort` cannot: the smallest line might arrive last,
so it must consume all its input before producing any output. `wc -l` is the same.

With a buffering program in the middle, the downstream program sits blocked on an
empty pipe for the entire run and then receives everything in a burst. Nothing is
wrong. That is what sorting is.

The kernel makes no distinction. All three are just tasks issuing reads and writes
and blocking on their own pipes. The difference is entirely in the program's logic,
and from outside a slow pipeline is indistinguishable from a hang.

## What this still is not

- **No file redirect.** `> OUT.TXT` needs a descriptor whose writes accumulate and
  flush on close, because the FAT32 layer only does whole-file writes. That is
  streaming file writes, a real new capability, and its own rung.
- **No `dup`.** No way to point two descriptors at one thing after the fact, so no
  `2>&1`.
- **No `select` or `poll`.** A task blocks on exactly one thing.
- **No SIGPIPE.** Writing to a pipe with no readers returns an error instead of
  killing the writer, which is why `yes | head -1` terminates on Unix.
- **The wake is a broadcast.** `scheduler_wake(WAIT_PIPE_READ)` wakes every task
  waiting on any pipe. They re-issue, find their own pipe still empty, and block
  again. Irrelevant with three tasks, a thundering herd with three thousand.
- **No named pipes.** A pipe cannot be given a name in the filesystem, so it can
  only ever be inherited from a parent.

## Exercises

1. `A's fds[1]` and `COUNT's fds[1]` both hold the number 1. Explain why they can
   route to different places, and name the line of kernel code that makes it so.
2. Write down, as precisely as you can, why a ring-3 program cannot determine
   whether its output is a pipe. Give the hardware reason, not the convention.
3. `kbd_buffer_push` drops on full and `pipe_write` blocks. Justify both, then say
   what would go wrong if each used the other's policy.
4. A reader is blocked on an empty pipe. The last writer exits. Trace what wakes the
   reader, and describe exactly what happens if `close_fd` does not wake.
5. Why is `writers` a count rather than a boolean? Give the specific moment in a
   two-stage pipeline when it is 2.
6. The shell closes its copies of both pipe ends immediately after spawning. Remove
   one of those closes and describe the resulting machine state, including what the
   user sees.
7. `sys_write` may return less than `len`. Write the loop a caller must use, and
   describe the bug in a caller that does not.
8. `pipe_read` blocks by calling `task_block` and returning without setting RAX.
   Explain what would happen if it set RAX to 0 first, and why the failure would
   appear to have nothing to do with pipes.
9. Two writes of 3 and 5 bytes are followed by a read of 4. Say exactly what the
   reader gets, and explain where the information about the original two calls went.
10. Sketch what `> OUT.TXT` would need, given that `fat32_write_file` takes a whole
    file at once. Name the piece of state a file descriptor would have to carry that
    a pipe descriptor does not.
