# MiniOS

A hobby operating system built from scratch in C and x86 assembly. Boots on real (virtualized) hardware, handles interrupts, manages memory, and runs a basic interactive shell.

## Features

- Custom bootloader
- GDT and IDT setup
- Interrupt handling (keyboard, timer)
- Physical and virtual memory management
- VGA text mode display driver
- PS/2 keyboard driver
- Custom libc (string, memory utilities)
- Basic interactive shell

## Tech Stack

- **Languages:** C, x86 Assembly (NASM)
- **Emulator:** QEMU
- **Build:** Make, LD (custom linker script)
- **Target:** x86 (32-bit protected mode)

## Building

```bash
make
```

## Running

```bash
make run
```

This launches the OS in QEMU.

## Project Structure

```
boot/       — bootloader (assembly)
kernel/     — core OS: GDT, IDT, interrupts, memory management
drivers/    — hardware drivers: screen, keyboard, I/O ports
libc/       — minimal C standard library
shell/      — interactive command shell
include/    — shared type definitions
```