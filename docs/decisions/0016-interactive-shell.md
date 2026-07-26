# 0016 - An interactive shell as a ring-3 program

## Status

Accepted.

## Context

MiniOS could load and run programs from the disk ([0015](0015-elf-program-loading.md)),
but there was no way to tell it what to run. The set of programs was a fixed list
in `kernel_main`, decided at kernel link time; the machine booted, ran three
letter-printers, and did exactly that forever. Nothing read input and acted on it.

There was an interactive shell, in `shell/shell.c`, but it was the wrong shape.
It ran in the kernel at ring 0: the keyboard IRQ called `shell_handle_keypress`
directly, which buffered the line and dispatched compiled-in command functions
(`help`, `clear`, `hello`, `tick`) that called kernel routines. It did real work
inside an interrupt handler, and it was privileged code, not a program. It was
also off the boot path, because `kernel_main` handed control to the scheduler
instead of ever calling `shell_init`.

The pieces a real shell needs already existed and were reachable only from the
kernel: keyboard decoding (`drivers/keyboard.c`), the directory walk
(`fs/fat32.c`), the file reader (`fat32_read_file`), and the loader
(`task_create_from_file`). A ring-3 program had no way to reach any of them,
because the only channel across the ring boundary was `int 0x50` and the only
calls behind it were `SYS_WRITE` and `SYS_EXIT`.

## Decision

Build the shell as a ring-3 program, `user/shell.c`, loaded off the disk as
`SHELL.ELF` like any other, and give it the four syscalls it needs to do its job.
The point is not just to have a shell: it is that a fully fenced-in program,
holding no privilege and touching nothing directly, can run an interactive shell
using nothing but syscalls. That is what proves the syscall boundary is complete.

**Four new syscalls**, each an entry in the `int 0x50` dispatcher
(`kernel/syscall.c`):

- `SYS_READKEY` (2): pop one character from the keyboard, or 0 if none is waiting.
- `SYS_LIST` (3): write the root directory's file names into a caller buffer.
- `SYS_RUN` (4): load and start a named program.
- `SYS_READFILE` (5): read a whole file into a caller buffer.

**A keyboard ring buffer** (`drivers/keyboard.c`). The keyboard IRQ no longer does
any real work: it decodes one scancode and pushes the character into a fixed
128-slot circular buffer, then returns. `SYS_READKEY` is the consumer that drains
it. A producer (the IRQ, running with interrupts masked) and a consumer (the
syscall, also with interrupts masked) that never overlap, connected by two indices
that chase each other around the ring, is the standard shape for handing data out
of an interrupt handler. A full buffer drops the newest key rather than
overwriting unread input.

**Non-blocking readkey.** On an empty buffer, `SYS_READKEY` returns 0 immediately
rather than blocking the caller until a key arrives. Blocking would need a way to
sleep a task and wake it from the IRQ, and this kernel has no task-sleeping. So the
shell busy-waits, polling `SYS_READKEY` in a loop, which burns a timeslice it could
have yielded. This is recorded as `TODO(blocking-readkey)`.

**Custom command names, not the Unix ones.** The commands are `list`, `read`,
`run`, `help`, `clear`, and `return`, deliberately not `ls`, `cat`, and `echo`.
They are the machine's own vocabulary.

**A reentrant tokenizer.** The shell splits a line into a command and an argument
with `next_token` (`user/userlib.h`), a `strtok_r`-style splitter that holds its
position in a caller pointer rather than a hidden global, skips leading separators
so repeated spaces do not make empty tokens, and shreds the line in place by
overwriting each separator with a NUL. It lives in the user runtime, not
`libc/string.c`, because the user build links no kernel objects (so
`libc/string.c` is unreachable from ring 3) and the kernel never tokenizes (so it
would be dead there).

**The old in-kernel shell is removed.** `shell/shell.c` and `shell/shell.h` are
deleted, along with the Makefile entry and the keyboard callback's call into it.
`kernel_main` now boots `SHELL.ELF` alone. The letter-printers stay on the disk so
the shell can start one with `run A.ELF`.

## Consequences

- **The machine is interactive, and the syscall boundary is proven complete.** A
  ring-3 program with no privilege runs a working shell. Every effect it has,
  reading a key, echoing it, listing the directory, printing a file, launching a
  program, goes through a syscall, because nothing else is available to it.
  Verified under QEMU by a scripted session (keys injected through the monitor,
  the VGA text buffer dumped after each command): the prompt appears at boot;
  `help` prints the command list; `list` prints the seven files on the disk;
  `read HELLO.TXT` prints `Hello from FAT32!`; `run A.ELF` prints `run: started
  a.elf` and then A's output interleaves with the live prompt, proving the
  launched program joined the scheduler; `return hello world` prints `hello
  world`; and `asdf` prints `unknown command: asdf` without faulting. `-d int`
  over the session showed only timer (`v=40`), keyboard (`v=41`), and syscall
  (`v=50`) vectors: no page fault (`0x0E`), no `#GP` (`0x0D`), no double fault
  (`0x08`), no triple fault, and no disk IRQ (`0x4E`, the polled driver stays
  silent).

- **The interrupt handler is short again.** The keyboard IRQ decodes one scancode
  and returns, which is what an interrupt handler running with interrupts masked
  should do. The line editing, echoing, and dispatch it used to do all moved to
  ring 3.

- **Every new syscall guards an untrusted pointer.** `SYS_LIST` and `SYS_READFILE`
  are handed buffers and `SYS_RUN` and `SYS_READFILE` are handed filenames, all
  from ring 3. Each is bounds-checked before the kernel touches it: buffers with
  `user_range_ok` (the whole `[ptr, ptr+len)` range, not just the start, and
  overflow-safe), filenames with `copy_user_string` (copied into the kernel with a
  length cap so a missing terminator cannot walk off the region). This is the same
  category of check as the loader's segment bounds and stricter than the
  `SYS_WRITE` stopgap it sits beside.

- **The shell busy-waits.** With non-blocking `SYS_READKEY`, an idle shell spins on
  the syscall gate, which is why `-d int` logs `v=50` millions of times over a few
  seconds of idle. It is correct but wasteful, and it is the one obvious thing a
  scheduler with sleeping would fix (`TODO(blocking-readkey)`).

- **No argv to launched programs.** `SYS_RUN` starts a program with the same empty,
  argument-less frame the loader always forges. The shell can say which program to
  run but cannot pass it anything, so `run` takes a filename and nothing more. Real
  arguments wait on a stack layout convention, the same gap [0015](0015-elf-program-loading.md)
  left.

- **No pipes, redirection, job control, or history.** The shell reads one line,
  runs one command, and prints to the screen. There is no way to connect two
  programs, redirect output, background a job, or recall a previous line. Backspace
  editing of the current line works; nothing beyond it does.

- **`clear` scrolls rather than clearing.** There is no clear-screen syscall (that
  would be a fifth syscall for a cosmetic command), so `clear` prints a screenful
  of newlines to scroll the old content off. The cursor ends near the bottom.

- **A failed `run` cannot take the kernel down.** `task_create_from_file` already
  reports and skips a missing or malformed program, so `SYS_RUN` returns -1 and the
  shell prints a message; the kernel keeps running.

## Related

- The syscall gate and convention this extends:
  [0007](0007-syscalls-via-int-0x50.md), [../reference/syscalls.md](../reference/syscalls.md).
- The loader `SYS_RUN` calls: [0015](0015-elf-program-loading.md),
  [../reference/elf-loading.md](../reference/elf-loading.md).
- The filesystem `SYS_LIST` and `SYS_READFILE` read through:
  [0014](0014-read-only-fat32.md), [../reference/fat32.md](../reference/fat32.md).
- The scheduler a launched program joins:
  [0008](0008-round-robin-preemptive-scheduler.md), [../reference/scheduling.md](../reference/scheduling.md).
- Reference page: [../reference/shell.md](../reference/shell.md).
