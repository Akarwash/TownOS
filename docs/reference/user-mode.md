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

`RFLAGS = 0x202` keeps the interrupt flag set. This is not optional: MiniOS has
no scheduler, so if ring 3 ran with interrupts masked the timer and keyboard
would go dead and the machine would look wedged.

`enter_user_mode` does not return. In the current build it is the last thing
`kernel_main` does.

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

The ring-3 program (`user/user_program.c`) does two things:

1. Writes a marker to a local variable, which lands on its ring-3 stack. This
   proves it is executing in its own pages.
2. Executes `cli`. That instruction is legal only at CPL 0; from ring 3 it
   raises a general protection fault (#GP, vector 0x0D). **The fault is the
   success signal** — a correct run never returns from `cli`.

Under QEMU with `-d int`, a correct run logs exactly:

```
v=0d e=0000 i=0 cpl=3 IP=001b:0000000000400019 ... env->regs[R_EAX]=0000000000001234
```

Reading it: `v=0d` is the #GP, `e=0000` its (empty) error code, `cpl=3` proves
ring 3, `IP=001b:...` shows CS = 0x1B with RIP inside `user_program`, and
`EAX=0x1234` is the stack marker, so the write happened. `check_exception old:
0xffffffff new 0xd` confirms it is a single fault, not a double/triple cascade.

### Isolation cross-check

Temporarily pointing the program's write at the kernel page `0x100000` instead
produces:

```
v=0e e=0007 i=0 cpl=3 ... CR2=0000000000100000
```

`v=0e` is a page fault; error code `0x7` decodes as present + write + **user**
(bit 2), and `CR2` is the kernel address the ring-3 write was denied. This is the
leaf-level user bit doing its job. The check is not part of the shipped program;
it is reverted to the `cli` test after confirming the behaviour.

## What this is not

- **Not a syscall path.** There is no controlled way back into the kernel yet;
  ring 3 re-enters the kernel only by faulting. A syscall gate (one DPL-3 IDT
  entry, or `syscall`/`sysret`) is the next step.
- **Not multitasking.** One ring-3 program runs, faults, and the kernel halts.
  Running a shell *and* user programs needs a scheduler.
- **Not per-process isolation.** There is a single shared address space. Every
  process would need its own page tables and a `CR3` switch on context switch.

## Related

- Decision and trade-offs: [../decisions/0006-user-mode-with-separate-pages.md](../decisions/0006-user-mode-with-separate-pages.md).
- The descriptors and TSS used: [gdt.md](gdt.md).
- The page tables re-privileged: [boot-sequence.md](boot-sequence.md).
- The faults that report success: [idt.md](idt.md).
- Memory layout and the 4-8MB overlap caveat: [memory-map.md](memory-map.md).
