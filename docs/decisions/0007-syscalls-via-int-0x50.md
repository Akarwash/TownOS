# 0007 - System calls via a single `int 0x50` gate

## Status

Accepted.

## Context

MiniOS can drop to ring 3 (see [decision 0006](0006-user-mode-with-separate-pages.md)),
but the only way ring-3 code could re-enter the kernel was by faulting. Every IDT
gate was DPL 0, so an `int N` from ring 3 raised a general protection fault before
any handler ran, and no `syscall`/`sysret` MSRs were programmed. A ring-3 program
could run, but it could not *ask* the kernel for anything: no output, no clean
exit, no service. That is the gap this decision closes.

Two mechanisms exist for a controlled user-to-kernel doorway on x86-64:

- A **software interrupt** through a dedicated IDT gate whose DPL allows ring 3
  (`int 0x50`). The CPU switches to the ring-0 stack from `tss.rsp0`, pushes the
  same frame any interrupt pushes, and vectors to a stub. This reuses machinery
  MiniOS already has: the IDT, the TSS's `rsp0`, and the `registers_t` stub
  layout every other vector already uses.
- The **`syscall`/`sysret`** fast instruction pair, which needs the `STAR`,
  `LSTAR`, and `SFMASK` MSRs programmed and a specific GDT selector ordering. It
  is faster (no memory-based descriptor lookup), but it is a separate code path
  with its own setup and none of it is wired up.

## Decision

**Use a single `int 0x50` IDT gate at DPL 3.** `SYSCALL_VECTOR` (0x50) already
had a reserved name in `include/vectors.h`; this makes it live. `kernel/isr.c`
installs exactly one gate with the flags byte `GATE_USER` (0xEE) = present, DPL 3,
64-bit interrupt gate. Every other gate stays DPL 0.

The DPL on a gate is the lowest privilege level allowed to reach it through
`int N`. Opening a second gate to ring 3 would be a hole: a DPL 3 gate on vector
0x0E would let a user program run `int $0x0E` to forge a page fault, and one on
0x40 to fake a timer tick, feeding the kernel lies about hardware state. So 0x50
is deliberately the *only* DPL 3 gate, and it stays an interrupt gate (not a trap
gate) so the CPU clears IF on entry and a syscall cannot be interrupted, including
by another syscall; they do not nest.

**Reuse the existing stub shape.** `syscall_stub` (`kernel/isr_stubs.asm`) mirrors
`ISR_NOERR`: a software interrupt pushes no error code, so it pushes a dummy 0
then the vector number, building the same `registers_t` frame every other stub
builds, and funnels into a shared tail that calls `syscall_handler(registers_t*)`.

**The ABI.** By convention here, RAX holds the syscall number, arguments follow in
RDI, RSI, RDX (System V order), and the return value comes back in RAX. The C
handler writes its result into the saved RAX slot of the frame; `POP_GPRS` then
restores that value into the real RAX, so `iretq` delivers it to the ring-3
caller. `syscall_handler` dispatches on RAX with a switch (`SYS_WRITE`,
`SYS_EXIT`); an unknown number is reported and rejected with `(uint64_t)-1`
rather than faulting or halting, because a bad request from ring 3 must never take
the kernel down.

**A standalone ABI header.** The syscall numbers live in `include/syscalls.h`,
deliberately numbers-only: no types, no declarations, no includes. A ring-3
program compiles against it without pulling in any kernel header, which matters
because kernel pages are not user-readable and a user program must not depend on
anything that lives there.

**Where the strings live.** The demo's string literals go in a new `.user_rodata`
section that `linker.ld` places in the 4-8M user region alongside `.user_text`. A
plain string literal would land in `.rodata` at 1M (kernel pages, not PG_USER),
where the pointer would both fail the syscall bounds check and fault a ring-3
read. The `int 0x50` wrappers in `user/user_program.c` are `always_inline`,
which is load-bearing at this project's `-O0`: a plain `static inline` is emitted
out of line into `.text` at 1M, and a ring-3 call into it faults; inlining folds
the trap directly into `user_program`, keeping every instruction in user pages.

## Consequences

- Ring-3 code can now request kernel services through one auditable doorway.
  `SYS_WRITE` prints a string; `SYS_EXIT` halts. The DPL-0-everywhere policy in
  the IDT gets its first, single, deliberate exception, and the reason is
  documented at the gate.
- **The `SYS_WRITE` pointer check is a stopgap, not real validation.** A ring-3
  pointer is untrusted (the confused-deputy problem: the kernel could be tricked
  into reading memory the caller cannot). The check only confirms the *start*
  pointer lies in the ring-3 region (`USER_REGION_START`..`USER_REGION_END`); it
  does **not** bound the string's length, so a string that starts just below
  `USER_REGION_END` with no NUL still walks out of the region into kernel pages.
  A real per-process range check waits on address spaces that do not exist yet.
  Recorded as a TODO in `kernel/syscall.c` and in
  [../project-status.md](../project-status.md).
- `SYS_EXIT` can only halt. With no scheduler and no parent to return to, "exit"
  has nowhere to go, so it stops the machine with `cli; hlt`, the same terminal
  state the exception handlers use.
- The frame the syscall stub builds is identical to every other interrupt's, so
  the same `registers_t` layout and the same push-order coupling with
  `kernel/isr.h` apply. Nothing new to keep in sync beyond the `SYSCALL_VECTOR`
  `equ` that `isr_stubs.asm` already duplicates from `vectors.h` by hand.
- `syscall`/`sysret` were not used. If syscall overhead ever matters, they are the
  next move, but they buy nothing for a kernel with no hot syscall path, and the
  `int 0x50` gate exercises machinery that already exists.

## Related

- The mechanism and ABI in depth: [../reference/syscalls.md](../reference/syscalls.md).
- The gate it adds and why every other gate stays DPL 0:
  [../reference/idt.md](../reference/idt.md).
- The ring-3 drop it complements: [decision 0006](0006-user-mode-with-separate-pages.md)
  and [../reference/user-mode.md](../reference/user-mode.md).
- The region bounds the pointer check reuses:
  [../reference/memory-map.md](../reference/memory-map.md).
