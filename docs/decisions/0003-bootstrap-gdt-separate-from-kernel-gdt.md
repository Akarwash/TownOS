# 0003 - Bootstrap GDT in boot.asm is separate from the kernel GDT

## Status

Accepted.

## Context

The far jump that switches the CPU into true 64-bit execution requires a valid
64-bit GDT to already be loaded, and that must happen inside `boot/boot.asm`
before any C code runs. The full kernel GDT (with user code/data descriptors and a
TSS) is built in C by `kernel/gdt.c`, which cannot run until after the far jump.
So some GDT has to exist earlier than the kernel GDT can be created.

## Decision

Keep two separate GDTs:

- A minimal **bootstrap GDT** lives in `boot/boot.asm`: a null descriptor, one
  64-bit code descriptor, and one flat data descriptor. Its only job is to make
  the far jump legal.
- The full **kernel GDT** is installed at runtime by `kernel/gdt.c` via
  `gdt_flush`, replacing the bootstrap GDT once C is running.

They are not shared or unified.

## Consequences

- Two GDTs exist in the tree, and this duplication is intentional, not an
  oversight. Each is commented to say so.
- The kernel GDT must keep kernel code at selector `0x08` and kernel data at
  `0x10`, matching the bootstrap GDT. If those selectors changed meaning between
  `lgdt` and the CS reload, the far return inside `gdt_flush` would fault. This
  constraint is load-bearing and is documented prominently in
  [../reference/gdt.md](../reference/gdt.md).
- The bootstrap GDT stays as simple as possible: it does not need user segments or
  a TSS, because nothing runs at ring 3 or takes an interrupt before the kernel
  GDT is installed.
