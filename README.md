# MiniOS

A hobby x86-64 operating system kernel built from scratch in C and assembly.

MiniOS boots via Multiboot, climbs the CPU into 64-bit long mode, sets up the GDT
and TSS, and is being built toward an interrupt-driven interactive shell. It is a
learning kernel: one address space, everything at ring 0, no processes and no
filesystem. The point is to understand how an operating system works by building a
small real one, not to be a production OS.

## Status

MiniOS **compiles and assembles but does not yet link**, so it does not run yet.
This is expected. The link fails on the interrupt entry symbols `isr0`-`isr31` and
`irq0`-`irq15`, which live in `kernel/isr_stubs.asm`. That file and `kernel/idt.c`
are the two remaining hand-written stubs. Full status is in
[docs/README.md](docs/README.md).

## Quick start

Install the toolchain (macOS, Homebrew):

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
```

Build:

```bash
make
```

For Linux instructions, exact versions, how to run under QEMU, and how to debug,
see [docs/building.md](docs/building.md).

## Documentation

MiniOS keeps two separate bodies of documentation:

- **[docs/](docs/README.md)** is project documentation: factual, derived from the
  source. What MiniOS is, how it is built, and why the load-bearing decisions were
  made. Start here to build or hack on the kernel.
- **[learnings/](learnings/README.md)** is conceptual learning material: how
  operating systems work in general, using MiniOS as the running example. Start
  here to learn the ideas.

They must not blur: `docs/` states facts about this codebase, `learnings/` teaches
concepts.

## Repository layout

```
boot/       Multiboot header and the 32 to 64 long-mode climb (assembly)
kernel/     GDT/TSS, IDT, interrupt dispatch, timer, memory allocator, kernel_main
drivers/    hardware drivers: screen, keyboard, I/O ports
libc/       minimal freestanding C library (string, mem)
shell/      interactive command shell
include/    shared type definitions
docs/       project documentation (factual)
learnings/  OS concepts and teaching material
linker.ld   section layout: kernel loads at 1M
Makefile    build system
```

See [docs/architecture.md](docs/architecture.md) for the file-by-file
responsibilities and the subsystem map.

## License

See [LICENSE](LICENSE).
