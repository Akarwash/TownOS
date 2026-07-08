# MiniOS

A hobby operating system built from scratch in C and x86 assembly. It boots via
Multiboot into 32-bit protected mode, sets up the GDT and IDT, handles hardware
interrupts (timer + keyboard), manages physical memory, and runs an interactive
shell — all in about 1,000 lines of code.

## Features

- Multiboot-compliant bootloader (`boot/boot.asm`)
- Flat-model GDT and full IDT with PIC remapping
- Interrupt dispatch: 32 CPU exceptions + 16 hardware IRQs
- PIT timer driver (100 Hz) on IRQ 0
- PS/2 keyboard driver on IRQ 1
- Physical memory frame allocator (bitmap)
- VGA text-mode screen driver with scrolling and a hardware cursor
- Minimal hand-written libc (`string`, `mem`)
- Interactive shell with `help`, `clear`, `hello`, `tick`

## Quick start

### 1. Install the toolchain (macOS, Homebrew)

MiniOS needs a **cross-compiler** that targets bare-metal 32-bit x86, an
assembler, and an emulator. All four are plain Homebrew formulae:

```bash
brew install i686-elf-gcc i686-elf-binutils nasm qemu
```

> Why a cross-compiler? Your Mac's built-in compiler produces binaries for macOS
> on your host CPU. A kernel needs code for a bare 32-bit x86 machine with no OS
> underneath. `i686-elf-gcc` produces exactly that. See
> [docs/07-build-system-and-toolchain.md](docs/07-build-system-and-toolchain.md).

### 2. Build

```bash
make
```

This compiles every C and assembly file, then links them with `linker.ld` into
`minios.bin` (an ELF image the Multiboot loader understands). You should see each
source compile with no warnings.

### 3. Run

```bash
make run
```

This launches the kernel in QEMU (`qemu-system-i386 -kernel minios.bin`). A window
opens showing:

```
Welcome to MiniOS!
> _
```

### 4. Try the shell

Type a command and press Enter. Backspace works.

| Command | What it does |
|---------|--------------|
| `help`  | list the available commands |
| `clear` | clear the screen |
| `hello` | print a greeting |
| `tick`  | print the timer tick count (increments 100×/second) |

Run `tick` twice a second apart — the number jumps by ~100, which proves the timer
interrupt (IRQ 0) is firing. Typing at all proves the keyboard interrupt (IRQ 1)
is working. Anything else prints `Unknown command: ...`.

To quit QEMU: close the window, or press `Ctrl-A` then `X` in the terminal if you
launched it there.

### Rebuild from scratch

```bash
make clean && make
```

## How it all works

This repo doubles as a guided course in operating systems. The
[`docs/`](docs/README.md) folder walks through every concept — booting, the GDT,
interrupts, drivers, memory, and the shell — using this code as the concrete
example. Start at [docs/README.md](docs/README.md), which includes a map of which
source file implements each idea.

Recommended reading order:

1. [What is an operating system?](docs/00-what-is-an-operating-system.md)
2. [How an OS boots](docs/01-how-an-os-boots.md)
3. [Protected mode and the GDT](docs/02-protected-mode-and-the-gdt.md)
4. [Interrupts](docs/03-interrupts.md) ← the heart of the system
5. [Drivers and I/O](docs/04-drivers-and-io.md)
6. [Memory management](docs/05-memory-management.md)
7. [The shell and event loop](docs/06-the-shell-and-event-loop.md)
8. [Build system and toolchain](docs/07-build-system-and-toolchain.md)

## Project structure

```
boot/       — Multiboot bootloader (assembly)
kernel/     — core OS: GDT, IDT, interrupts, timer, memory, kernel_main
drivers/    — hardware drivers: screen, keyboard, I/O ports
libc/       — minimal C standard library (string, mem)
shell/      — interactive command shell
include/    — shared type definitions
docs/        — learning guide (start here to understand the code)
linker.ld    — memory layout: kernel loads at 1 MB
Makefile     — build system
```

## Tech stack

- **Languages:** C, x86 assembly (NASM)
- **Toolchain:** `i686-elf-gcc` cross-compiler, `i686-elf-ld`, GNU Make
- **Emulator:** QEMU (`qemu-system-i386`)
- **Target:** x86, 32-bit protected mode
