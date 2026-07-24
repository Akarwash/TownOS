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
> Note: the chapters fall into three states, shown in the index below.
> **Frozen-32bit:** chapters 1, 2, 3, and 5 were written for the original 32-bit
> protected-mode design and describe it, not the current kernel (MiniOS is now
> x86-64 long mode). They are kept as personal learning material and are
> deliberately **not** rewritten; where such a chapter and
> [`../docs/`](../docs/README.md) disagree on a fact, `../docs/` is the current
> truth. **Written:** chapters that still hold. **Stubbed:** chapters 08 through 15
> cover subsystems the kernel grew after the 32-bit era (long mode, the 64-bit
> GDT/TSS, user mode, syscalls, the scheduler, the heap, per-process paging, the
> disk driver). Each of these has a factual [`../docs/reference/`](../docs/README.md)
> page today but no hand-written teaching chapter yet, so it exists only as a
> stub: a title, a scope sentence, a status marker, and a link to its reference
> page. The teaching prose for these is written by hand, later; the stubs mark the
> gap honestly instead of hiding it.

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

## The chapters: an honest index

This index lists every concept the current kernel implements, the state of its
teaching chapter, and the factual reference page that already covers it. The three
chapter states are:

- **written** — a teaching chapter that still describes the current kernel.
- **frozen-32bit** — a chapter written for the original 32-bit design, kept as-is,
  superseded on the x86-64 details by its reference page (and by a stub below).
- **stubbed** — a title, scope sentence, status marker, and reference link only;
  the teaching prose is written by hand, later.

| # | Concept | Chapter | State | Reference page (facts, now) |
|---|---------|---------|-------|------------------------------|
| 0 | What an OS is, the kernel/user split | [00-what-is-an-operating-system.md](00-what-is-an-operating-system.md) | written | (pure concept) |
| 1 | Booting: power-on to `kernel_main` | [01-how-an-os-boots.md](01-how-an-os-boots.md) | frozen-32bit | [../docs/reference/boot-sequence.md](../docs/reference/boot-sequence.md) |
| 2 | Protected mode, segmentation, the GDT | [02-protected-mode-and-the-gdt.md](02-protected-mode-and-the-gdt.md) | frozen-32bit | [../docs/reference/gdt.md](../docs/reference/gdt.md) |
| 3 | Interrupts: the IDT, the PIC, ISRs, EOI | [03-interrupts.md](03-interrupts.md) | frozen-32bit | [../docs/reference/idt.md](../docs/reference/idt.md) |
| 4 | Drivers and port I/O: screen, keyboard, timer | [04-drivers-and-io.md](04-drivers-and-io.md) | written | (screen/keyboard/timer have no dedicated reference page) |
| 5 | Memory: physical, virtual, allocators | [05-memory-management.md](05-memory-management.md) | frozen-32bit | [../docs/reference/memory-map.md](../docs/reference/memory-map.md) |
| 6 | The shell and the event loop | [06-the-shell-and-event-loop.md](06-the-shell-and-event-loop.md) | written | (see [../docs/architecture.md](../docs/architecture.md)) |
| 7 | Build system and toolchain | [07-build-system-and-toolchain.md](07-build-system-and-toolchain.md) | written | [../docs/building.md](../docs/building.md) |
| 8 | Long mode and paging (the x86-64 climb) | [08-long-mode-and-paging.md](08-long-mode-and-paging.md) | stubbed | [boot-sequence.md](../docs/reference/boot-sequence.md), [paging.md](../docs/reference/paging.md), [memory-map.md](../docs/reference/memory-map.md) |
| 9 | The 64-bit GDT and the TSS | [09-the-64bit-gdt-and-tss.md](09-the-64bit-gdt-and-tss.md) | stubbed | [../docs/reference/gdt.md](../docs/reference/gdt.md) |
| 10 | User mode (ring 3) | [10-user-mode.md](10-user-mode.md) | stubbed | [../docs/reference/user-mode.md](../docs/reference/user-mode.md) |
| 11 | System calls | [11-system-calls.md](11-system-calls.md) | stubbed | [../docs/reference/syscalls.md](../docs/reference/syscalls.md) |
| 12 | The scheduler | [12-the-scheduler.md](12-the-scheduler.md) | stubbed | [../docs/reference/scheduling.md](../docs/reference/scheduling.md) |
| 13 | The heap | [13-the-heap.md](13-the-heap.md) | stubbed | [../docs/reference/heap.md](../docs/reference/heap.md) |
| 14 | Per-process paging | [14-per-process-paging.md](14-per-process-paging.md) | stubbed | [../docs/reference/paging.md](../docs/reference/paging.md) |
| 15 | The disk driver | [15-the-disk-driver.md](15-the-disk-driver.md) | stubbed | [../docs/reference/disk.md](../docs/reference/disk.md) |
| 16 | The filesystem | [16-the-filesystem.md](16-the-filesystem.md) | written | [../docs/reference/fat32.md](../docs/reference/fat32.md) |

Chapters 1, 2, 3, and 5 (frozen-32bit) and chapters 8, 9, and 14 (stubbed)
overlap on purpose: the frozen chapter teaches the original 32-bit take on boot,
the GDT, interrupts, and memory, while the new stub will teach the current
x86-64 form of the same subsystem. The frozen chapter is kept for its 32-bit
narrative; the current facts live in the reference page and, eventually, the stub.

Supporting material, not chapters:

| File | What it is |
|------|------------|
| [glossary.md](glossary.md) | Every acronym and term, defined in one place |
| [lecture.md](lecture.md) | Long-form lecture notes covering the same material |
| [learning.md](learning.md) | Personal running notes gathered while building MiniOS |

## A map of MiniOS

Keep this next to you. It shows which source file implements each concept.

```
boot/boot.asm .............. Multiboot header + first instructions   → ch.1, ch.8
kernel/kernel.c ............ kernel_main: the "init" sequence          → ch.6
kernel/gdt.c / gdt_flush.asm  Global Descriptor Table + TSS           → ch.2, ch.9
kernel/idt.c ............... Interrupt Descriptor Table + PIC remap    → ch.3
kernel/isr.c / isr_stubs.asm  Interrupt handlers (C side + asm stubs)  → ch.3
kernel/timer.c ............. PIT timer driver (IRQ0)                    → ch.4
kernel/memory.c ............ physical memory allocator                 → ch.5
kernel/paging.c ............ per-process page tables, CR3 switch        → ch.8, ch.14
kernel/heap.c .............. kmalloc/kfree (explicit free list)         → ch.13
kernel/usermode.c .......... drop to ring 3 (forged iretq frame)        → ch.10
kernel/syscall.c ........... the int 0x50 syscall dispatcher            → ch.11
kernel/scheduler.c ......... round-robin preemptive scheduler          → ch.12
drivers/screen.c ........... VGA text-mode output                      → ch.4
drivers/keyboard.c ......... PS/2 keyboard input (IRQ1)                → ch.4
drivers/disk.c ............. polled ATA PIO disk driver                 → ch.15
fs/fat32.c ................. read-only FAT32 filesystem                 → ch.16
drivers/ports.c ............ in/out instruction wrappers               → ch.4
user/user_program.c ........ the ring-3 demo programs                   → ch.10
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
