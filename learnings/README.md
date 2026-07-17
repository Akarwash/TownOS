# Learning Operating Systems with MiniOS (conceptual material)

This folder is **conceptual learning material**. It teaches how an operating
system works, using **MiniOS** (the little x86-64 kernel in this repository) as a
running example. Every abstract idea (interrupts, memory, drivers) is tied back
to a real file you can open and read.

The goal is not just "understand this codebase." It is **understand operating
systems as a whole**, and use MiniOS as the concrete thing your intuition can
grab onto.

> **`learnings/` vs [`../docs/`](../docs/README.md).** These are two different
> things and must not blur. `learnings/` explains OS *concepts* and teaches the
> *why*; it is allowed to be discursive and to describe things in general terms.
> [`../docs/`](../docs/README.md) is *project documentation*: strictly factual,
> derived from the actual source, describing what MiniOS is and how to build,
> run, and reason about it today. To build or hack on the kernel, start at
> [`../docs/`](../docs/README.md). To learn how operating systems work, stay
> here.
>
> Note: chapters 1, 2, 3, and 5 were written for the original 32-bit
> protected-mode design and describe it, not the current kernel. MiniOS is now
> x86-64 long mode. These chapters are kept as personal learning material and are
> deliberately **not** rewritten; each one carries a note at the top pointing to
> the current reference page. Where a chapter and [`../docs/`](../docs/README.md)
> disagree on a fact, `../docs/` is the current truth.

## Who this is for

You know C. You are comfortable with pointers, structs, and bitwise operators.
You have seen a little assembly but do not need to be fluent. You are curious
about what actually happens between pressing the power button and a blinking
cursor.

## How to read this

Read the chapters in order the first time — each one builds on the last. Later,
use them as reference. Each chapter follows the same shape:

1. **The big idea** — the concept as it exists in *all* operating systems (Linux,
   Windows, macOS, a microwave's firmware). This is the part that transfers.
2. **How the hardware forces the design** — why the concept exists at all. Most
   OS design is not arbitrary; it is the software shape of a hardware constraint.
3. **How MiniOS does it** — the specific files, functions, and magic numbers,
   with pointers into the source.
4. **Going further** — how real, production kernels extend the idea, plus
   exercises.

## The chapters

| # | File | What you'll learn |
|---|------|-------------------|
| 0 | [00-what-is-an-operating-system.md](00-what-is-an-operating-system.md) | What an OS actually is, the kernel/user split, and the four jobs every kernel does |
| 1 | [01-how-an-os-boots.md](01-how-an-os-boots.md) | Power-on to `kernel_main`: firmware, bootloaders, Multiboot, real vs protected mode |
| 2 | [02-protected-mode-and-the-gdt.md](02-protected-mode-and-the-gdt.md) | Segmentation, privilege rings, and why MiniOS builds a "flat" GDT |
| 3 | [03-interrupts.md](03-interrupts.md) | The single most important OS mechanism: IDT, the PIC, ISRs, IRQs, and EOI |
| 4 | [04-drivers-and-io.md](04-drivers-and-io.md) | Talking to hardware: port I/O, the VGA screen, PS/2 keyboard, and the PIT timer |
| 5 | [05-memory-management.md](05-memory-management.md) | Physical vs virtual memory, allocators, paging, and where MiniOS stops |
| 6 | [06-the-shell-and-event-loop.md](06-the-shell-and-event-loop.md) | Tying it together: the event loop, `hlt`, and how a keypress becomes a command |
| 7 | [07-build-system-and-toolchain.md](07-build-system-and-toolchain.md) | Freestanding C, cross-compilers, linker scripts, and how bytes become a bootable image |
| — | [glossary.md](glossary.md) | Every acronym and term, defined in one place |
| — | [lecture.md](lecture.md) | Long-form lecture notes covering the same material |
| — | [learning.md](learning.md) | Personal running notes gathered while building MiniOS |

## A map of MiniOS

Keep this next to you. It shows which source file implements each concept.

```
boot/boot.asm .............. Multiboot header + first instructions   → ch.1
kernel/kernel.c ............ kernel_main: the "init" sequence          → ch.6
kernel/gdt.c / gdt_flush.asm  Global Descriptor Table (segmentation)  → ch.2
kernel/idt.c ............... Interrupt Descriptor Table + PIC remap    → ch.3
kernel/isr.c / isr_stubs.asm  Interrupt handlers (C side + asm stubs)  → ch.3
kernel/timer.c ............. PIT timer driver (IRQ0)                    → ch.4
kernel/memory.c ............ physical memory allocator                 → ch.5
drivers/screen.c ........... VGA text-mode output                      → ch.4
drivers/keyboard.c ......... PS/2 keyboard input (IRQ1)                → ch.4
drivers/ports.c ............ in/out instruction wrappers               → ch.4
libc/string.c, mem.c ....... hand-rolled standard library              → ch.7
shell/shell.c .............. the interactive command loop              → ch.6
include/types.h ............ fixed-width integer types                 → ch.7
linker.ld .................. memory layout of the final binary         → ch.7
```

> Note: all of these files are now implemented and the kernel boots. The factual
> state of what is implemented lives in the project documentation at
> [`../docs/`](../docs/README.md). These chapters explain the *why* behind the
> code so that when you read each file, you understand what it is doing and not
> just what it says.

## The one-paragraph summary of everything

A computer starts in a dumb 16-bit mode running firmware. The firmware (or a
bootloader like GRUB) loads your kernel into memory and jumps to it. Your kernel
climbs the CPU into 64-bit long mode, sets up tables that describe memory
(the GDT) and how to handle interrupts (the IDT), programs the interrupt
controller so hardware can get the CPU's attention, and then installs drivers for
the screen, keyboard, and timer. Finally it enters an idle loop and does nothing
until an interrupt — a keypress, a timer tick — wakes it up to do work. That loop,
woken by interrupts, *is* the operating system. Everything else is detail.
