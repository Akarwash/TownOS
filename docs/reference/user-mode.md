# User mode (ring 3)

This page documents how MiniOS drops to CPL 3 and how it proves the drop is
real. Read from `kernel/usermode.c`, `user/user_program.c`, `boot/boot.asm`,
`linker.ld`, and `kernel/gdt.c`. For the rationale and trade-offs see
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

## What runs where

| Piece | Location | Source |
|-------|----------|--------|
| Ring-3 program code | `0x400000` (in `.user_text`, PD[2]) | `user/user_program.c`, `linker.ld` |
| Ring-3 stack top | `0x800000` (top of PD[3]) | `USER_STACK_TOP` in `kernel/usermode.h` |
| Ring-0 stack on entry | `tss.rsp0` (top of `tss_stack`) | `kernel/gdt.c` |
| Ring-3 code selector | `0x1B` (`GDT_SELECTOR_USER_CODE`, RPL 3) | `kernel/gdt.h` |
| Ring-3 data selector | `0x23` (`GDT_SELECTOR_USER_DATA`, RPL 3) | `kernel/gdt.h` |

## The transition

There is no instruction that raises code into ring 3. x86 only *lowers*
privilege by returning into less-privileged code, so `enter_user_mode`
(`kernel/usermode.c`) forges the frame an interrupt-return consumes and executes
it. In 64-bit mode `iretq` pops five 8-byte values in this order:

```
RIP, CS, RFLAGS, RSP, SS
```

so the function pushes them in reverse (SS first, RIP last), then runs `iretq`:

```
push  SS      = 0x23   (ring-3 data selector)
push  RSP     = 0x800000
push  RFLAGS  = 0x202  (reserved bit 1 + IF)
push  CS      = 0x1B   (ring-3 code selector, RPL 3)
push  RIP     = &user_program
iretq
```

Because the popped CS has RPL 3, the CPU performs a privilege change: it loads
`SS:RSP`, sets RFLAGS, and begins executing at `CS:RIP` as ring-3 code. The data
segment registers (DS/ES/FS/GS) are pointed at the ring-3 data selector first;
long mode largely ignores them for addressing, but leaving ring-0 selectors
loaded across the drop is untidy.

`RFLAGS = 0x202` keeps the interrupt flag set. This is not optional: if ring 3 ran
with interrupts masked the timer and keyboard would go dead, and (now that a
scheduler exists) the running task would never be preempted, owning the machine
forever. The same 0x202 is what `task_create` forges into every task's saved
frame. See [scheduling.md](scheduling.md).

`enter_user_mode` does not return. It is still the path into ring 3, now invoked
by `scheduler_start` (`kernel/scheduler.c`) to enter task 0; every later entry
into a task goes through the scheduler restoring its saved frame instead.

## Why the isolation holds

The x86 page walk grants ring-3 access only if the user (US) bit is set at
*every* level from PML4 to the leaf — the bits are ANDed down the walk. MiniOS
uses that: the two upper levels are permissive and the PD leaves decide access.

```
PML4[0], PDPT[0]          user bit SET     (permissive: a user branch may pass)
PD[0]  0x000000-0x1FFFFF  user bit CLEAR   kernel code, data, VGA, stack
PD[1]  0x200000-0x3FFFFF  user bit CLEAR   kernel
PD[2]  0x400000-0x5FFFFF  user bit SET     ring-3 code
PD[3]  0x600000-0x7FFFFF  user bit SET     ring-3 stack
```

Setting the user bit high on PML4[0]/PDPT[0] does not expose the kernel: the
PD[0]/PD[1] leaves still withhold it, and a walk that hits a clear user bit at
any level denies ring-3 access. The leaf is the real gate.

## How it is proven

The drop was first proven by making the ring-3 program execute `cli`, a
CPL-0-only instruction, and observing the #GP it raises at `cpl=3` with
`CS=0x1B`; an isolation cross-check pointing a ring-3 write at the kernel page
`0x100000` produced a #PF with the user error-code bit set and `CR2=0x100000`.
That verification, with the exact QEMU `-d int` output, is recorded in
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

The shipped programs (`user/user_program.c`) no longer fault on purpose. Now that
a syscall gate and a scheduler exist, they demonstrate the drop the constructive
way: three of them run at CPL 3 and call the kernel through `int 0x50` (`SYS_WRITE`)
in a loop, and the scheduler switches between them on the timer tick. Each task
runs in its OWN page-table tree (per-process paging), so they share the same user
virtual addresses but not the same physical memory. Under QEMU with `-d int`,
vector `0x50` fires from all three tasks (three distinct RIPs, each in its own
address space with its own `CR3`), each at `cpl=3`, with no #GP and no #PF; the
three strings interleave on screen, printed by the kernel on the ring-3 programs'
behalf. That a ring-3 pointer into `.user_rodata` (4-8M) is accepted while a kernel
address is rejected is the same leaf-level user-bit boundary, now exercised through
the syscall path instead of a fault. See [syscalls.md](syscalls.md),
[scheduling.md](scheduling.md), and [paging.md](paging.md).

## What this is not

- **Not full multitasking.** Three hard-coded ring-3 tasks are preempted
  round-robin by the timer (see [scheduling.md](scheduling.md)), but there is no
  process abstraction and no program loading: the tasks are compiled into the
  kernel image, not loaded from a filesystem.
- **Not demand paging.** Each task now has real per-process isolation: its own
  page-table tree and a `CR3` switch on every context switch, so tasks share
  virtual addresses but not physical frames (see [paging.md](paging.md)). What is
  still missing is laziness: every page is mapped eagerly at `task_create`, with no
  page-fault-driven demand paging, copy-on-write, or swapping.

## Related

- Decision and trade-offs: [../decisions/0006-user-mode-with-separate-pages.md](../decisions/0006-user-mode-with-separate-pages.md).
- The descriptors and TSS used: [gdt.md](gdt.md).
- The page tables re-privileged: [boot-sequence.md](boot-sequence.md).
- The faults that report success: [idt.md](idt.md).
- Memory layout and the 4-8MB overlap caveat: [memory-map.md](memory-map.md).
