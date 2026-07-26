# Shell reference

MiniOS boots into an interactive shell that is a ring-3 program, `SHELL.ELF`,
loaded off the disk like any other. It reads typed commands and runs them using
nothing but syscalls: it holds no privilege and touches the keyboard, screen,
filesystem, and loader only through the `int 0x50` gate. This page documents the
read-match-do loop, the keyboard ring buffer behind `SYS_READKEY`, the four
syscalls the shell needs, the tokenizer, and the command table. Read from
`user/shell.c`, `user/userlib.h`, `drivers/keyboard.c`, and `kernel/syscall.c`.
For the rationale, see [decision 0016](../decisions/0016-interactive-shell.md).

## The command table

The names are MiniOS's own and deliberately not the Unix ones.

| Command | Argument | Effect |
|---------|----------|--------|
| `list` | none | List the files in the root directory. |
| `read` | a filename | Print that file's contents. |
| `run` | a filename | Load and start that program. |
| `help` | none | Print the command list. |
| `clear` | none | Clear the screen (scroll it away with newlines). |
| `return` | text | Print the text back. |

An empty line does nothing. Any other first word prints `unknown command: <word>`.
Filenames are 8.3 and case-insensitive, so `read hello.txt` finds `HELLO.TXT`.

## The read-match-do loop

`_start` (`user/shell.c`) prints a prompt and then loops:

1. **Read a line.** `read_line` builds the line one keystroke at a time by calling
   `SYS_READKEY` in a loop. A printable character is appended to a fixed line
   buffer and echoed with `SYS_WRITE` so the user sees it. Enter (`\n`) ends the
   line. Backspace (`\b`) removes the last character and erases it on screen, and
   is guarded so it cannot chew back into the prompt. The line buffer is fixed at
   `SHELL_LINE_MAX` (128); once full, further printable characters are dropped
   rather than overflowed.
2. **Tokenize.** `next_token` splits the line on spaces, in place. The first token
   is the command; a second token, where a command takes one, is the argument.
3. **Match and do.** A chain of string compares dispatches to one of the commands
   above. `list`, `read`, and `run` call the syscalls below; `help`, `clear`, and
   `return` are handled with `SYS_WRITE` alone.
4. Reprint the prompt and loop.

Because `SYS_READKEY` is non-blocking (see below), step 1 is a busy-wait: the shell
spins on the syscall until a key arrives. That is the deliberate cost of having no
task-sleeping in the kernel (`TODO(blocking-readkey)`).

## The keyboard ring buffer

The keyboard is a producer/consumer queue. The producer is the keyboard IRQ; the
consumer is `SYS_READKEY`. They are connected by a fixed circular buffer in
`drivers/keyboard.c`.

**The producer.** The keyboard IRQ (`keyboard_callback`) runs with interrupts
masked and must be short, so it does the least possible work: it reads one
scancode, ignores key-release events, decodes the make code to ASCII, and pushes
the character into the ring buffer. It does not echo and does not run any command.
Before the shell became a ring-3 program, this callback called
`shell_handle_keypress` and did the whole line edit and dispatch inside the
interrupt; the ring buffer is what keeps that out of interrupt context now.

**The consumer.** `SYS_READKEY` (`kernel/syscall.c`) calls `keyboard_getchar`,
which pops one character from the ring, or returns 0 if it is empty.

**The two indices.** `write_index` is the slot the next produced character goes
into; `read_index` is the slot the next consumed character comes out of. They chase
each other around `KBD_BUFFER_SIZE` (128) slots, each advance stepping forward one
slot and wrapping modulo the size.

- `write_index == read_index` means **empty**.
- To keep empty distinguishable from full with that one rule, one slot is always
  left unused: the ring holds at most `KBD_BUFFER_SIZE - 1` characters. If a full
  buffer were allowed to wrap `write_index` onto `read_index`, full would look
  exactly like empty.
- **Full-drop policy.** If advancing `write_index` would land on `read_index`, the
  ring is full and the new character is dropped, not stored. Dropping a keypress
  the user can retype is the lesser evil; overwriting input already accepted, and
  collapsing the empty/full distinction, is worse.

**No lock is needed.** The producer runs in the keyboard IRQ and the consumer in
the syscall handler; both the IRQ gate and the syscall gate clear IF on entry, so
on a single CPU the two never run at the same instant and the indices need no
guard.

**The empty sentinel.** 0 is a safe "nothing waiting" value because the scancode
table maps every unmapped key to 0 and the producer only pushes non-zero
characters, so a real 0 never enters the ring.

## The four syscalls

All four follow the calling convention in [syscalls.md](syscalls.md): RAX carries
the number in and the result out, arguments in RDI/RSI/RDX. The ring-3 wrappers are
in `user/userlib.h`.

| Number | Name | Arguments | Returns |
|--------|------|-----------|---------|
| 2 | `SYS_READKEY` | none | one character, or 0 if none waiting |
| 3 | `SYS_LIST` | RDI = buffer, RSI = size | number of names, or -1 |
| 4 | `SYS_RUN` | RDI = filename pointer | 0 on success, -1 on failure |
| 5 | `SYS_READFILE` | RDI = filename, RSI = buffer, RDX = size | bytes read, or -1 |

- **`SYS_READKEY`** pops one buffered key (above). Non-blocking: it returns 0
  immediately on an empty buffer rather than sleeping the caller, because there is
  no task-sleeping. `TODO(blocking-readkey)`.
- **`SYS_LIST`** walks the FAT32 root directory (through `fat32_list_names`, the
  buffer-filling sibling of `fat32_list_root`) and writes the file names into the
  caller's buffer, one per line, NUL-terminated. Names that do not all fit are
  dropped from the end. Returns the count.
- **`SYS_RUN`** copies the filename into the kernel and calls
  `task_create_from_file`, which loads the program into a fresh address space and
  registers it with the scheduler. The launched program joins the round-robin and
  interleaves with the shell; the shell keeps running. A missing or malformed
  program is reported and skipped, so a failed run returns -1 and never faults the
  kernel.
- **`SYS_READFILE`** reads a whole file (through `fat32_read_file`) into the
  caller's buffer and returns the byte count. The bytes are raw and not
  NUL-terminated; the shell terminates them before printing the buffer as a
  string. `read` needs this because the shell is ring 3 and cannot call
  `fat32_read_file` itself.

**The untrusted pointers.** Every pointer these take comes from ring 3 and is
checked before the kernel touches it. Buffers go through `user_range_ok`, which
confirms the whole `[ptr, ptr+len)` range lies in the ring-3 region
(`USER_REGION_START`..`USER_REGION_END`) and is careful about overflow (a crafted
length that would make `ptr+len` wrap is caught by comparing `len` against the room
above `ptr`, not by forming the sum). Filenames go through `copy_user_string`,
which bounds-checks the start pointer and copies with a length cap, so a string
with no terminator cannot walk out of the region. This is the same security
boundary as the loader's segment check and stricter than the `SYS_WRITE` stopgap
in the same file.

## The tokenizer

`next_token` (`user/userlib.h`) splits a string on a separator, in place, and is
reentrant.

```
char *next_token(char **pos, char sep);
```

- **Reentrant (`strtok_r` style).** The caller holds the current position in
  `*pos`; there is no hidden global. This deliberately avoids `strtok`'s single
  static cursor, which makes it non-reentrant: a second tokenization, even inside a
  called function, clobbers the first.
- **In place, no copying.** The separator after a token is overwritten with a NUL
  and the returned pointer points into the caller's buffer, so **the input buffer
  is modified**. That is what lets the shell match a token with a plain string
  compare and still reach the untouched remainder of the line (which is how
  `return` echoes the rest of the line).
- **Skips leading separators.** A run of separators before a token is stepped over,
  so `run   A.ELF` (extra spaces) yields `run` then `A.ELF`, never empty tokens in
  between. This is the non-obvious part, and the reason an empty or all-separator
  remainder returns null (no more tokens) rather than a zero-length token.

It lives in the user runtime rather than `libc/string.c` because the user build
compiles a single freestanding translation unit and links no kernel objects, so
`libc/string.c` is unreachable from ring 3, and the kernel never tokenizes, so it
would be dead code there. See
[decision 0016](../decisions/0016-interactive-shell.md).

## Building and running the shell

`user/shell.c` is built exactly like the other user programs (`-mcmodel=small`,
freestanding, static, linked at 0x400000 with `user/user.ld`) but with an explicit
Makefile rule, since its source is lowercase `shell.c` and its on-disk name is
uppercase `SHELL.ELF`. `make run` copies it onto the image, and `kernel_main`
launches it. See [../building.md](../building.md).

## Related

- The syscall gate, convention, and the `SYS_WRITE` stopgap this sits beside:
  [syscalls.md](syscalls.md), [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The loader `SYS_RUN` calls: [elf-loading.md](elf-loading.md),
  [decision 0015](../decisions/0015-elf-program-loading.md).
- The filesystem `SYS_LIST` and `SYS_READFILE` read through:
  [fat32.md](fat32.md), [decision 0014](../decisions/0014-read-only-fat32.md).
- The scheduler a launched program joins: [scheduling.md](scheduling.md).
- The decision behind all of this: [decision 0016](../decisions/0016-interactive-shell.md).
