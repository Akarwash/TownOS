# System call reference

MiniOS lets a ring-3 program request kernel services through a single software
interrupt, `int 0x50`. This page documents the gate, the calling convention, the
two calls that exist, and the one thing that is deliberately not yet safe. Read
from `kernel/syscall.c`, `kernel/isr_stubs.asm`, `kernel/isr.c`,
`include/syscalls.h`, and `user/user_program.c`. For the rationale and the
alternatives considered, see
[decision 0007](../decisions/0007-syscalls-via-int-0x50.md).

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

`SYS_EXIT` can only halt: with no scheduler and no parent to return to, there is
nowhere for an exit to go, so it stops the machine, the same terminal state the
exception handlers use.

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

## The ring-3 side

`user/user_program.c` shows the caller's half. The raw `int 0x50` is wrapped in
`always_inline` helpers (`sys_write`, `sys_exit`) built on inline asm with
explicit register constraints (`"a"` = RAX, `"D"` = RDI), and `SYSCALL_VECTOR`
reaches the `int` instruction as an immediate through an `"i"` constraint so the
vector stays a named constant. `always_inline` is not decoration: at this
project's `-O0`, a plain `static inline` is emitted out of line into `.text` at
1M (kernel pages), and a ring-3 call into it faults; inlining folds the trap into
`user_program`, keeping every instruction in the program's own user pages.

The strings the program prints live in a `.user_rodata` section, which `linker.ld`
places in the 4-8M user region alongside `.user_text`. A plain string literal
would land in `.rodata` at 1M (kernel pages), where the pointer would both fail
the bounds check above and fault a ring-3 read.

## What a run looks like

The demo program calls `SYS_WRITE` twice with different strings, then `SYS_EXIT`.
Booted under QEMU with `-d int`, vector `0x50` fires three times, each at `cpl=3`,
with no `#GP` (0x0D) and no `#PF` (0x0E). On screen:

```
user: hello from ring 3, via int 0x50
user: and a second syscall, still in ring 3
syscall: SYS_EXIT, halting.
```

Passing a kernel address (for example `0x100000`) to `SYS_WRITE` instead prints
`syscall: SYS_WRITE rejected an out-of-bounds pointer` and returns `-1`, printing
nothing from kernel memory, confirming the stopgap check fires.

## Related

- Why one DPL 3 gate and not `syscall`/`sysret`:
  [decision 0007](../decisions/0007-syscalls-via-int-0x50.md).
- The IDT gate, the flags byte, and the stub shape:
  [idt.md](idt.md).
- The ring-3 drop that precedes any syscall:
  [user-mode.md](user-mode.md) and
  [decision 0006](../decisions/0006-user-mode-with-separate-pages.md).
- The region the pointer check reuses: [memory-map.md](memory-map.md).
