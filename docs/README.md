# MiniOS project documentation

This is the factual documentation for MiniOS: what it is, how it is put together,
how to build and run it, and why the load-bearing decisions were made. It is
derived from the source, not from concepts. For the conceptual "how operating
systems work" material, see [`../learnings/`](../learnings/README.md) instead.
The two are kept separate on purpose: `docs/` states facts about this codebase,
`learnings/` teaches ideas.

## Pages

| Page | What it covers |
|------|----------------|
| [architecture.md](architecture.md) | What MiniOS is, the directory layout, the subsystem map, and control flow from `_start` to the event loop |
| [building.md](building.md) | Toolchain and versions, install commands, how to build and run, and how to debug |
| [reference/boot-sequence.md](reference/boot-sequence.md) | The 32 to 64 long-mode climb in `boot/boot.asm`, step by step |
| [reference/memory-map.md](reference/memory-map.md) | Physical memory layout: load address, VGA buffer, identity-mapped region, page tables, stacks |
| [reference/gdt.md](reference/gdt.md) | The kernel GDT and TSS: selector table, descriptor layouts, and the bootstrap-vs-kernel GDT split |
| [decisions/](decisions/) | Architecture decision records (ADRs) for the load-bearing choices |

Reference page pending: `reference/idt.md`. The IDT is not documented yet because
`kernel/idt.c` is still a stub (see Status below). It will be written once the IDT
install path exists.

## Decisions

- [0001 — Target x86-64 rather than i686](decisions/0001-target-x86-64.md)
- [0002 — Use 2MB pages and identity-map the first 8MB](decisions/0002-2mb-pages-and-8mb-identity-map.md)
- [0003 — Bootstrap GDT separate from the kernel GDT](decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md)
- [0004 — Build the TSS before user mode exists](decisions/0004-build-tss-before-user-mode.md)

## Status

MiniOS **compiles and assembles but does not yet link**, and therefore does not
run. This is expected, not a broken setup.

- All C sources compile cleanly under `-Wall -Wextra`.
- All assembly sources assemble cleanly with `nasm -f elf64`.
- The link fails on undefined symbols `isr0`-`isr31` and `irq0`-`irq15`, which
  are the interrupt entry points that belong in `kernel/isr_stubs.asm`. That file
  and `kernel/idt.c` are the two remaining `TODO(long-mode)` stubs.

What is implemented: the Multiboot header and the 32 to 64 long-mode climb
(`boot/boot.asm`), the kernel GDT and TSS (`kernel/gdt.c`, `kernel/gdt_flush.asm`),
the PIC remap and IDT zeroing (`kernel/idt.c`, partial), and all the portable C
(drivers, libc, timer, memory allocator, shell). See
[architecture.md](architecture.md) for the full implemented-vs-stub map.

For the exact build, run, and debug commands, see [building.md](building.md).
