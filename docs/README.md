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
| [reference/heap.md](reference/heap.md) | The kernel heap: header/footer boundary tags, split/coalesce, the frame-allocator seam, and interrupt safety |
| [project-status.md](project-status.md) | What works, what was never built, and the natural next steps |
| [decisions/](decisions/) | Architecture decision records (ADRs) for the load-bearing choices |

## Decisions

- [0001 — Target x86-64 rather than i686](decisions/0001-target-x86-64.md) — Build for 64-bit long mode from the start rather than 32-bit i686.
- [0002 — Use 2MB pages and identity-map the first 8MB](decisions/0002-2mb-pages-and-8mb-identity-map.md) — Identity-map the first 8MB with 2MB pages, so only three page-table levels are needed (no PT).
- [0003 — Bootstrap GDT in boot.asm is separate from the kernel GDT](decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md) — A throwaway bootstrap GDT makes the far jump legal; C installs the real kernel GDT later, and the two stay separate.
- [0004 — Build the TSS now, before user mode exists](decisions/0004-build-tss-before-user-mode.md) — Build the TSS (descriptor, ring-0 stack, `ltr`) now, before user mode needs it.
- [0005 — A self-describing interrupt vector map](decisions/0005-self-describing-vector-map.md) — Assign vectors by category (exceptions 0x00-0x1F, IRQs 0x40-0x4F, syscalls 0x50-0x5F), with `include/vectors.h` the single source of truth.
- [0006 — Enter ring 3 with a separate user page region](decisions/0006-user-mode-with-separate-pages.md) — Drop to ring 3 through a forged `iretq` frame, marking the 4-8M pages user while kernel pages stay ring-0-only.
- [0007 — System calls via a single `int 0x50` gate](decisions/0007-syscalls-via-int-0x50.md) — Route every syscall through one DPL 3 `int 0x50` gate (the only user-reachable gate); RAX carries the call number.
- [0008 — A round-robin preemptive scheduler](decisions/0008-round-robin-preemptive-scheduler.md) — Preempt on the timer tick by overwriting the interrupt frame in place, with a fixed `.bss` task table of four.
- [0009 — Read the Multiboot memory map and extend the identity map](decisions/0009-read-multiboot-map-extend-identity-map.md) — Read the Multiboot map, extend the identity map to real RAM (capped at 1GB), and size the frame pool from it.
- [0010 — Port the p5 explicit-free-list allocator as the kernel heap](decisions/0010-kernel-heap-ported-from-p5.md) — Port the CMSC216 p5 `el_malloc` (boundary tags, coalescing) as `kmalloc`/`kfree`, swapping `mmap` for `alloc_frames_contiguous` and adding an interrupt guard.
- [0011 — Heap-allocate task structs and bump-allocate user stacks](decisions/0011-dynamic-tasks-and-stacks.md) — Retire the fixed four-task array (`kmalloc` each `task_t`) and the two hardcoded stacks (bump-allocate 256KB slices of the user region); the stack ceiling remains until per-process paging.

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
