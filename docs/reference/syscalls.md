# System call reference

MiniOS lets a ring-3 program request kernel services through a single software
interrupt, `int 0x50`. This page documents the gate, the calling convention, the
six calls that exist, and the pointer checks that guard the untrusted ones. Read
from `kernel/syscall.c`, `kernel/isr_stubs.asm`, `kernel/isr.c`,
`include/syscalls.h`, `drivers/keyboard.c`, and `user/userlib.h`. For the
rationale and the alternatives considered, see
[decision 0007](../decisions/0007-syscalls-via-int-0x50.md). The four calls the
interactive shell needs (`SYS_READKEY`, `SYS_LIST`, `SYS_RUN`, `SYS_READFILE`) are
covered here and in [shell.md](shell.md); see
[decision 0016](../decisions/0016-interactive-shell.md).

## The doorway

There is exactly one syscall entry point: IDT vector `SYSCALL_VECTOR` (0x50),
installed by `isr_install()` in `kernel/isr.c` with the flags byte `GATE_USER`
(0xEE) = present, **DPL 3**, 64-bit interrupt gate. The DPL is what lets ring-3
`int 0x50` reach the gate; every other gate is DPL 0, so this is the only vector
a user program can raise on purpose. It is an interrupt gate, not a trap gate, so
the CPU clears IF on entry: a syscall runs with interrupts masked and cannot be
nested by another syscall or an IRQ.

When ring-3 code runs `int 0x50`, the CPU switches to the ring-0 stack from
`tss.rsp0`, pushes the usual interrupt frame, and vectors to `syscall_stub`
(`kernel/isr_stubs.asm`). The stub pushes a dummy error code (a software
interrupt carries none) and the vector number, saves all 15 general-purpose
registers, and calls `syscall_handler(registers_t*)` with the frame pointer in
RDI. This is the exact same shape as every other interrupt stub; the syscall path
adds no new stack layout.

## Calling convention

| Register | Role |
|----------|------|
| RAX | syscall number (in), return value (out) |
| RDI | first argument |
| RSI | second argument |
| RDX | third argument |

This is System V argument order, with RAX carrying the call number in and the
result out. `syscall_handler` (`kernel/syscall.c`) switches on `regs->rax` and
writes the result back into `regs->rax`; when the stub runs `POP_GPRS` and
`iretq`, the caller finds the return value in RAX.

The numbers themselves live in `include/syscalls.h`, which is deliberately
standalone: numbers and nothing else, no types, no declarations, no includes. A
ring-3 program can include it without pulling in any kernel header, which it must
not do (kernel pages are not user-readable).

## The calls

| Number | Name | Arguments | Returns | Effect |
|--------|------|-----------|---------|--------|
| 0 | `SYS_EXIT` | none | does not return | Halts the machine with `cli; hlt`. |
| 1 | `SYS_WRITE` | RDI = pointer to a NUL-terminated string | 0 on success, `(uint64_t)-1` if rejected | Prints the string via the VGA driver. |
| 2 | `SYS_READKEY` | none | one character, or 0 if none waiting | Pops one key from the keyboard ring buffer. |
| 3 | `SYS_LIST` | RDI = buffer, RSI = size | number of names, or -1 | Writes the root directory's file names into the buffer, newline-separated. |
| 4 | `SYS_RUN` | RDI = filename pointer | 0 on success, -1 on failure | Loads and starts the named program as a new task. |
| 5 | `SYS_READFILE` | RDI = filename, RSI = buffer, RDX = size | bytes read, or -1 | Reads a whole file into the buffer. |

`SYS_EXIT` can only halt: with no scheduler and no parent to return to, there is
nowhere for an exit to go, so it stops the machine, the same terminal state the
exception handlers use.

`SYS_READKEY` is the consumer end of the keyboard ring buffer (`drivers/keyboard.c`,
documented in [shell.md](shell.md)). It is **non-blocking**: on an empty buffer it
returns 0 immediately rather than sleeping the caller, because the kernel has no
task-sleeping. A caller polls it in a loop (`TODO(blocking-readkey)`). 0 is a safe
"nothing waiting" sentinel because a real 0 never enters the buffer.

`SYS_LIST`, `SYS_RUN`, and `SYS_READFILE` are the shell's data calls. `SYS_LIST`
fills a buffer through `fat32_list_names`; `SYS_READFILE` fills one through
`fat32_read_file`; `SYS_RUN` copies in a filename and calls
`task_create_from_file`, which loads the program and registers it with the
scheduler (a failed load returns -1 and never faults the kernel). Each takes an
untrusted pointer from ring 3, checked as described below.

An **unknown syscall number** is not fatal. `syscall_handler` prints the offending
number and returns `(uint64_t)-1` in RAX; a bad request from ring 3 must never
fault or halt the kernel.

## The untrusted pointer, and why the check is a stopgap

`SYS_WRITE` receives a pointer from ring 3, and that pointer is **untrusted**. A
ring-3 program could pass a kernel address and turn the kernel into a confused
deputy, printing memory the program itself is not allowed to read.

The current check is a stopgap and is documented as one. It confirms only that
the *start* pointer falls inside the ring-3 region
(`USER_REGION_START`..`USER_REGION_END`, i.e. 4-8M, the same constants
`kernel/memory.c` reserves, exposed in `kernel/memory.h`). Anything outside is
rejected with `(uint64_t)-1` and nothing is printed.

What it does **not** do: it does not bound the string's *length*. A string that
starts just below `USER_REGION_END` with no NUL terminator still walks out of the
region and into kernel pages, and this check would not catch it. Proper
validation means checking the whole `[ptr, ptr+len)` range against the caller's
own mapped pages and capping the length, which needs per-process address-space
tracking that does not exist yet. This is recorded as a TODO in
`kernel/syscall.c` and in [../project-status.md](../project-status.md). Do not
mistake the region check for real pointer validation.

## The shell syscalls bound the whole range

The four calls added for the shell take untrusted pointers too, and they do better
than the `SYS_WRITE` stopgap: they bound the entire range, not just the start.
`kernel/syscall.c` has two shared helpers.

`user_range_ok(ptr, len)` confirms that all of `[ptr, ptr+len)` lies inside the
ring-3 region before the kernel writes a byte through the pointer. It is careful
about overflow: `ptr + len` can wrap on a crafted length and a wrapped sum compares
as comfortably small, so `len` is checked against the room above `ptr`
(`USER_REGION_END - ptr`) rather than by forming the sum. `SYS_LIST` and
`SYS_READFILE` bound their destination buffers with it.

`copy_user_string(ptr, dst, cap)` copies a NUL-terminated string in from ring 3
with a length cap, so a string with no terminator cannot walk off the region: it
bounds-checks the start pointer, then copies until a NUL, until the cap, or until
`USER_REGION_END`, whichever comes first, and always NUL-terminates. `SYS_RUN` and
`SYS_READFILE` copy their filenames in with it.

This is the same category of check as the ELF loader's segment bounds
([elf-loading.md](elf-loading.md)) and stricter than the `SYS_WRITE` stopgap above.
It still checks virtual addresses against the fixed region constants rather than
walking the caller's page tables, so it is not yet the full per-process validation
the `SYS_WRITE` TODO describes, but it does bound the whole range and cap the
length, which the stopgap does not.

## The ring-3 side

`user/userlib.h` shows the caller's half. The raw `int 0x50` is wrapped in
`always_inline` helpers built on inline asm with explicit register constraints
(`"a"` = RAX, `"D"` = RDI, `"S"` = RSI, `"d"` = RDX), one per arity: `syscall0`
through `syscall3`, with `sys_write`, `sys_exit`, `sys_readkey`, `sys_list`,
`sys_run`, and `sys_readfile` over them. `SYSCALL_VECTOR` reaches the `int`
instruction as an immediate through an `"i"` constraint so the vector stays a named
constant. `always_inline` is kept: it folds the trap
directly into the caller, so every instruction the program runs is inside its own
mapped text and there is no call through a symbol the (relocation-free) loader
would have to resolve. It used to be load-bearing for a sharper reason, back when
an out-of-line helper could land in kernel pages at 1M and fault a ring-3 call.

The strings the program prints need no special section any more. A program is now
linked on its own at 0x400000 (`user/user.ld`), so its ordinary `.rodata` already
lands in the 4-8M user region where a ring-3 pointer is allowed to point. When the
programs were compiled into the kernel, a plain string literal would have landed
in the kernel's `.rodata` at 1M, where the pointer would both fail the bounds
check above and fault a ring-3 read; that is why the old build forced them into a
`.user_rodata` section by hand.

## What a run looks like

The machine boots into `SHELL.ELF`, which prints a prompt and loops on
`SYS_READKEY`, echoing with `SYS_WRITE`, tokenizing each line, and dispatching the
commands in [shell.md](shell.md). Booted under QEMU and driven with a scripted key
sequence, it behaves like this:

```
> help
commands:
  list           list files in the root directory
  ...
> list
HELLO.TXT
TEST.TXT
BIG.TXT
A.ELF
B.ELF
C.ELF
SHELL.ELF
> read hello.txt
Hello from FAT32!
> run a.elf
run: started a.elf
> AAAAAAAAAA...
```

`run a.elf` calls `SYS_RUN`, which loads `A.ELF` as a new task; from the next timer
tick A's `SYS_WRITE` output interleaves with the live prompt, which is the
scheduler running the shell and A together. Over the session `-d int` shows only
timer (`v=40`), keyboard (`v=41`), and syscall (`v=50`) vectors, all at `cpl=3` for
the ring-3 traps, with no `#GP` (0x0D), no `#PF` (0x0E), no double fault (0x08), no
triple fault, and no disk IRQ (0x4E). The idle shell's busy-wait on `SYS_READKEY`
is why `v=50` dominates the log.

Passing a kernel address (for example `0x100000`) to `SYS_WRITE` still prints
`syscall: SYS_WRITE rejected an out-of-bounds pointer` and returns `-1`, and the
shell syscalls reject an out-of-region buffer or filename the same way, printing
nothing from kernel memory. (`SYS_EXIT` stays implemented; it halts with
`cli; hlt`, but the shell never calls it.)

## Related

- Why one DPL 3 gate and not `syscall`/`sysret`:
  [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The IDT gate, the flags byte, and the stub shape:
  [idt.md](idt.md).
- The ring-3 drop that precedes any syscall:
  [user-mode.md](user-mode.md) and
  [decision 0006](../decisions/0006-user-mode-with-separate-pages.md).
- The region the pointer check reuses: [memory-map.md](memory-map.md).
- The shell that uses `SYS_READKEY`/`SYS_LIST`/`SYS_RUN`/`SYS_READFILE`, and the
  keyboard ring buffer behind `SYS_READKEY`: [shell.md](shell.md) and
  [decision 0016](../decisions/0016-interactive-shell.md).
