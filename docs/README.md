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
| [reference/idt.md](reference/idt.md) | The IDT and interrupt entry path: gate format, PIC remap, the 48 stubs, dispatch, and EOI |
| [project-status.md](project-status.md) | What works, what was never built, and the natural next steps |
| [decisions/](decisions/) | Architecture decision records (ADRs) for the load-bearing choices |

## Decisions

- [0001 — Target x86-64 rather than i686](decisions/0001-target-x86-64.md)
- [0002 — Use 2MB pages and identity-map the first 8MB](decisions/0002-2mb-pages-and-8mb-identity-map.md)
- [0003 — Bootstrap GDT separate from the kernel GDT](decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md)
- [0004 — Build the TSS before user mode exists](decisions/0004-build-tss-before-user-mode.md)

## Status

MiniOS **builds, links, and boots to an interactive shell** under QEMU.

- All C sources compile cleanly under `-Wall -Wextra`.
- All assembly sources assemble cleanly with `nasm -f elf64`.
- The kernel links into `minios.elf` and is repackaged as a Multiboot-loadable
  `minios.bin`. `make run` boots it under QEMU: the banner and prompt appear, the
  timer ticks on IRQ 0, the keyboard delivers keypresses on IRQ 1, and the shell
  runs `help`, `clear`, `hello`, and `tick`.

The full feature list, the things that were deliberately never built (user mode,
syscalls, a scheduler, per-process paging, a filesystem), and the natural next
steps are in [project-status.md](project-status.md).

For the exact build, run, and debug commands, see [building.md](building.md).
