# 0006 - Enter ring 3 with a separate user page region

## Status

Accepted. Partly superseded — the ring-3 drop stands, where the ring-3 code comes
from does not.

- **`user/user_program.c`, `.user_text`, and `.user_rodata`** were superseded by
  [0015](0015-elf-program-loading.md). The body describes a ring-3 function
  compiled into the kernel image and placed at `0x400000` by `linker.ld`. That
  file, those sections and those linker symbols no longer exist: programs are
  separate ELF files read off the disk at runtime.
- **A single shared user region** was superseded by
  [0012](0012-per-process-paging.md). The 4-8MB user region is no longer one set
  of pages every ring-3 thing shares; each task has a private tree, and the
  region's physical frames are reserved from the allocator rather than executed
  out of.

The `iretq` forge, the selectors and RPL 3, `RFLAGS = 0x202`, the TSS `rsp0`
requirement, and the user bit as the isolation boundary are all unchanged. See
[reference/user-mode.md](../reference/user-mode.md) for the current state.

## Context

Everything in MiniOS ran at ring 0. The GDT already carried user code and data
descriptors (DPL 3) and the TSS already held a ring-0 stack pointer in `rsp0`,
but nothing ever entered ring 3, so both were inert scaffolding (see
[decision 0004](0004-build-tss-before-user-mode.md)). The next real step for a
kernel is to actually run code at CPL 3 and prove the privilege boundary is
real, not just declared.

Two things had to be decided: how the low privilege gets entered, and where the
ring-3 code and its stack live in memory.

Dropping privilege is not symmetric with raising it. x86 offers no "jump to ring
3" instruction. Privilege can only be *lowered by returning* into less-privileged
code, so the entry has to be forged as a return.

For memory, the point of user mode is isolation: ring-3 code must be able to
touch its own pages and must *not* be able to touch the kernel's. The x86 page
walk decides user access by ANDing the user (US, bit 2) bit at every level from
the PML4 down to the leaf. That AND is the lever: a branch is only reachable from
ring 3 if every level along it permits user access, so where the user bit is set
and where it is withheld is the whole isolation policy.

## Decision

**Entry.** Add `enter_user_mode(entry, stack_top)` (`kernel/usermode.c`). It
loads the ring-3 data selector into the data segment registers, forges the exact
five-value frame `iretq` pops in 64-bit mode (SS, RSP, RFLAGS, CS, RIP, pushed
high to low), and executes `iretq`. Because the pushed CS has RPL 3, the CPU
performs a privilege change and begins executing the target as ring-3 code. The
pushed RFLAGS keeps the interrupt flag (IF) set: MiniOS has no scheduler, so
entering ring 3 with interrupts masked would silence the timer and keyboard and
wedge the machine.

**Page layout.** The four PD entries that identity-map the first 8MB now carry
different privileges, and the two upper levels are made permissive:

```
PML4[0], PDPT[0]          present + writable + USER   (permissive: user may pass)
PD[0]  0x000000-0x1FFFFF  present + writable          kernel: code, data, VGA, stack
PD[1]  0x200000-0x3FFFFF  present + writable          kernel
PD[2]  0x400000-0x5FFFFF  present + writable + USER    ring-3 program code
PD[3]  0x600000-0x7FFFFF  present + writable + USER    ring-3 program stack
```

The upper levels say "user may continue down this branch"; the leaves decide who
actually reaches each 2MB page. PD[0]/PD[1] withhold the user bit, so the kernel
stays ring-0-only even though the branch above it is permissive. This is the
standard shape: permissive at the top, restrictive at the leaf.

The ring-3 program is a small self-contained function (`user/user_program.c`)
placed in a new `.user_text` section, which `linker.ld` locates at `0x400000`
inside PD[2]. It references no kernel symbol (it could not call one: the kernel
pages are not user-accessible). Its stack top is `0x800000`, the top of PD[3].

**Proof it is really ring 3.** The program executes `cli`, which is legal only
at CPL 0. From ring 3 it raises a #GP (vector 0x0D). The fault *is* the success
signal: it fires at `cpl=3` with `CS=0x1B`, and the existing diagnostic handler
reports it and halts. An isolation cross-check (temporarily writing to the kernel
page at `0x100000`) produces a #PF (vector 0x0E) with error code bit 2 (user)
set and `CR2=0x100000`, confirming the leaf-level user bit is what gates access.

## Consequences

- The user GDT descriptors and `tss.rsp0` are no longer inert: a real privilege
  drop uses the ring-3 selectors, and an interrupt taken in ring 3 switches to
  `rsp0`. Decision 0004's groundwork is now load-bearing.
- `.user_text` sits at `0x400000`, 3MB above the kernel at 1M. A separate
  `PT_LOAD` segment (declared via `PHDRS` in `linker.ld`) keeps that gap out of
  the file, so `minios.bin` stays ~25KB instead of ballooning by 3MB. The cost
  is an explicit `PHDRS` block and a linker warning about an RWX load segment,
  which is cosmetic here.
- The user region (4-8MB) **overlaps the frame allocator's pool**, which
  `kernel/memory.c` starts at 4M. Nothing allocates a frame in the demo path
  (the kernel drops to ring 3 immediately after init, and the allocator only
  returns addresses without touching memory), so this is not an active bug, but
  it is a latent collision that a real user/VM layer must resolve. Recorded in
  [../reference/memory-map.md](../reference/memory-map.md).
- `enter_user_mode` does not return. In the current demo it is the last thing
  `kernel_main` does: the shell's idle loop is unreachable once we drop to ring 3,
  because the ring-3 program faults and the handler halts. Running both a shell
  and user programs needs a scheduler, which does not exist yet.
- The entry uses `iretq` with a hand-built frame, the same mechanism a syscall
  return or a context switch will use. This is the reusable half of the work; the
  throwaway half is the demo program that deliberately faults.

## Related

- The mechanism, in depth: [../reference/user-mode.md](../reference/user-mode.md).
- The descriptors and TSS it activates: [../reference/gdt.md](../reference/gdt.md)
  and [decision 0004](0004-build-tss-before-user-mode.md).
- The page tables it re-privileges: [../reference/boot-sequence.md](../reference/boot-sequence.md)
  and [decision 0002](0002-2mb-pages-and-8mb-identity-map.md).
- The faults that prove it: [../reference/idt.md](../reference/idt.md).
