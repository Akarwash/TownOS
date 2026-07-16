# Architecture

MiniOS is a small x86-64 hobby kernel that boots via Multiboot, climbs into
64-bit long mode, and runs an interrupt-driven interactive shell. It is a
learning kernel: single address space, everything at ring 0, no processes and no
filesystem.

This page is a map, not a tutorial. For the concepts behind each subsystem, see
[`../learnings/`](../learnings/README.md) and follow the cross-links.

## Directory layout

| Directory | Responsibility |
|-----------|----------------|
| `boot/` | Multiboot header and the hand-written 32 to 64 long-mode climb (assembly). |
| `kernel/` | Core kernel: GDT/TSS, IDT, interrupt dispatch, timer, physical memory allocator, and `kernel_main`. |
| `drivers/` | Hardware drivers: VGA text screen, PS/2 keyboard, port I/O helpers. |
| `libc/` | Minimal freestanding C library: `string` and `mem` routines. |
| `shell/` | The interactive command shell. |
| `include/` | Shared definitions (`types.h`, the fixed-width integer types). |

## Source files

| File | Responsibility | State |
|------|----------------|-------|
| `boot/boot.asm` | Multiboot header, page tables, PAE/EFER/paging, bootstrap GDT, far jump to 64-bit, call `kernel_main`. | Implemented |
| `kernel/kernel.c` | `kernel_main`: the init sequence and the idle loop. | Implemented |
| `kernel/gdt.c`, `kernel/gdt.h` | Kernel GDT (null, kernel code/data, user code/data) and 64-bit TSS. | Implemented |
| `kernel/gdt_flush.asm` | `lgdt`, reload data segments, reload CS via far return, `ltr`. | Implemented |
| `kernel/idt.c`, `kernel/idt.h` | IDT table, `idt_set_entry`, PIC remap, IDT zeroing. | Partial: `idt_set_entry` body and `lidt` are stubs |
| `kernel/isr.c`, `kernel/isr.h` | C-side interrupt dispatch: `isr_install`, `isr_handler`, `irq_handler`, handler registration. | Implemented (depends on the asm stubs below) |
| `kernel/isr_stubs.asm` | `isr0`-`isr31`, `irq0`-`irq15` entry points and the common save/restore stubs. | Stub (empty) |
| `kernel/timer.c`, `kernel/timer.h` | PIT driver: program channel 0, count ticks on IRQ 0. | Implemented |
| `kernel/memory.c`, `kernel/memory.h` | Bitmap physical frame allocator. | Implemented |
| `drivers/screen.c`, `drivers/screen.h` | VGA text output: `print_char`/`print_string`/`print_int`, scrolling, cursor. | Implemented |
| `drivers/keyboard.c`, `drivers/keyboard.h` | PS/2 keyboard driver on IRQ 1, scancode to ASCII. | Implemented |
| `drivers/ports.c`, `drivers/ports.h` | `in`/`out` port I/O wrappers. | Implemented |
| `libc/string.c`, `libc/string.h` | `strlen`, `strcmp`, `strcpy`. | Implemented |
| `libc/mem.c`, `libc/mem.h` | `memcpy`, `memset`. | Implemented |
| `shell/shell.c`, `shell/shell.h` | Command loop: buffer keypresses, dispatch `help`/`clear`/`hello`/`tick`. | Implemented |
| `include/types.h` | Fixed-width integer types and `NULL`. | Implemented |
| `linker.ld` | Section layout, kernel loads at 1M. | Implemented |
| `Makefile` | Build rules, toolchain, flags. | Implemented |

## Subsystem map

The boot-to-shell chain, with implementation state:

```
boot (boot.asm) ............ implemented   long-mode climb, hands off to kernel_main
  -> GDT/TSS (gdt.c) ....... implemented   segment descriptors + TSS, ltr
  -> IDT (idt.c) ........... partial       PIC remap + IDT zero done; set_entry/lidt stubbed
  -> ISR stubs (isr_stubs)   stub          isr0-31 / irq0-15 not written -> link fails here
  -> drivers ............... implemented   screen, keyboard, timer, ports
  -> shell event loop ...... implemented   waits on interrupts, dispatches commands
```

The kernel builds and assembles but does not link: `kernel/isr.c` references the
`isr0`-`isr31` / `irq0`-`irq15` symbols that `kernel/isr_stubs.asm` has not
defined yet. See [building.md](building.md) for the exact failure and
[README.md](README.md) for status.

## Control flow

From power-on to the idle loop:

1. GRUB/QEMU reads the Multiboot header in `boot/boot.asm` and hands control to
   `_start` in 32-bit protected mode.
2. `_start` performs the 32 to 64 long-mode climb (page tables, PAE, EFER.LME,
   enable paging), loads a bootstrap GDT, and far-jumps into 64-bit code. See
   [reference/boot-sequence.md](reference/boot-sequence.md).
3. The 64-bit entry sets `RSP = stack_top` and calls `kernel_main`.
4. `kernel_main` (`kernel/kernel.c`) runs the init sequence in order:
   `gdt_init()`, `isr_install()`, `timer_init(100)`, `keyboard_init()`,
   `memory_init()`, then clears the screen, prints the banner, and calls
   `shell_init()`.
5. `kernel_main` enters an idle loop of `hlt`. The CPU sleeps until an interrupt
   (timer tick on IRQ 0, keypress on IRQ 1) wakes it. Keypresses drive the shell;
   the event loop woken by interrupts is the running system.

`kernel_main` takes no arguments and is not expected to return.

## Where to read more

- Boot climb: [reference/boot-sequence.md](reference/boot-sequence.md)
- GDT/TSS: [reference/gdt.md](reference/gdt.md)
- Memory layout: [reference/memory-map.md](reference/memory-map.md)
- Concepts (the why): [`../learnings/`](../learnings/README.md)
