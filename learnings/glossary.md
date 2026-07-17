# Glossary

Every acronym and term used in these docs, defined in one place. The chapter in
brackets is where the term is explained in depth.

## Boot and modes

- **Bootstrap / boot** — the chain of progressively more capable programs that each
  load the next, from firmware to your kernel. [ch.1]
- **Firmware (BIOS/UEFI)** — the code baked into the motherboard that runs first at
  power-on and hands off to a bootloader. [ch.1]
- **Bootloader** — a program (e.g. GRUB) that loads the kernel into memory, sets up
  the CPU, and jumps to it. In MiniOS, QEMU's `-kernel` plays this role. [ch.1]
- **Multiboot** — a specification: a contract between a bootloader and a kernel. A
  kernel with the Multiboot header (magic `0x1BADB002`) will be loaded into 32-bit
  protected mode with a known state. [ch.1]
- **Real mode** — the primitive 16-bit CPU mode at power-on: ~1 MB addressable, no
  protection. [ch.1]
- **Protected mode** — 32-bit CPU mode with memory protection and privilege levels;
  what MiniOS runs in. [ch.1, ch.2]
- **Long mode** — 64-bit CPU mode; retires most of segmentation. [ch.2]
- **`_start`** — the linker-designated entry point; the first instruction executed
  (in `boot.asm`). [ch.1]

## Memory and protection

- **GDT (Global Descriptor Table)** — the table of segment descriptors the CPU uses
  to interpret memory. [ch.2]
- **Descriptor** — one entry in the GDT/IDT describing a segment or a gate. [ch.2, ch.3]
- **Selector** — a value loaded into a segment register that indexes the GDT (e.g.
  `0x08` = kernel code, `0x10` = kernel data). [ch.2]
- **Segmentation** — the older x86 memory scheme where addresses go through segment
  registers and the GDT. [ch.2]
- **Flat memory model** — defining all segments as base 0, limit 4 GB so
  segmentation effectively disappears; used by MiniOS and most modern OSes. [ch.2]
- **Privilege rings (0–3)** — hardware privilege levels. Ring 0 = kernel, ring 3 =
  user. MiniOS uses only ring 0. [ch.0, ch.2]
- **Granularity bit** — a descriptor flag that makes the limit count 4 KB pages
  instead of bytes, allowing a 20-bit limit to span 4 GB. [ch.2]
- **`__attribute__((packed))`** — a GCC directive that forbids the compiler from
  inserting padding in a struct, so its layout matches the exact byte order the
  hardware requires. Used on every hardware-facing struct. [ch.2, ch.3]
- **Physical memory** — actual RAM, addressed from 0. MiniOS uses it directly. [ch.5]
- **Virtual memory** — the per-process illusion of a private, contiguous address
  space, translated to physical memory by the MMU. MiniOS does not implement it. [ch.5]
- **Paging** — translating virtual → physical in fixed 4 KB pages via page tables. [ch.5]
- **Page table / page directory** — the in-memory tree that describes virtual →
  physical mappings; pointed at by `CR3`. [ch.5]
- **MMU (Memory Management Unit)** — the hardware that performs address translation
  on every access. [ch.5]
- **TLB (Translation Lookaside Buffer)** — a cache of recent virtual→physical
  translations. [ch.5]
- **Frame** — a fixed-size (4 KB) chunk of physical memory; the unit a physical
  allocator hands out. [ch.5]
- **Bitmap allocator** — tracks free/used frames with one bit each. MiniOS's
  physical allocator. [ch.5]
- **Heap allocator (`kmalloc`)** — a byte-granular allocator built on top of a
  frame allocator. [ch.5]
- **`.bss`** — the binary section for zero-initialised data; takes no file space and
  is zeroed at load. Holds MiniOS's stack. [ch.1, ch.7]

## Interrupts

- **Interrupt** — a forced diversion of CPU execution to a handler, with the ability
  to resume exactly where it left off. [ch.3]
- **Exception / fault** — an interrupt the CPU raises about itself (divide by zero,
  page fault). Interrupt numbers 0–31. [ch.3]
- **IRQ (Interrupt Request)** — a hardware interrupt from a device. [ch.3]
- **Software interrupt** — an interrupt triggered by the `int N` instruction; the
  classic system-call mechanism. [ch.3]
- **IDT (Interrupt Descriptor Table)** — the table mapping interrupt numbers to
  handler addresses. [ch.3]
- **Gate** — an IDT entry; an *interrupt gate* auto-disables interrupts while its
  handler runs. [ch.3]
- **PIC (Programmable Interrupt Controller, 8259)** — the chip(s) that route device
  IRQs to the CPU. MiniOS remaps them to vectors 0x40–0x4F (chapter 3 says 32–47;
  the vector map has since changed, see `docs/decisions/0005`). [ch.3]
- **PIC remapping** — reprogramming the PIC so its IRQs arrive as vectors 0x40–0x4F,
  avoiding collision with CPU exceptions 0–31. The single source of truth for
  every vector number is `include/vectors.h`. [ch.3]
- **APIC / IO-APIC** — the modern multicore replacement for the 8259 PIC. [ch.3]
- **ISR (Interrupt Service Routine)** — the handler code that runs on an interrupt.
  In MiniOS, `isr_stubs.asm` (assembly entry points) + `isr.c` (C handlers). [ch.3]
- **Stub** — a small assembly entry point per interrupt that normalises the stack
  and calls a shared C handler. [ch.3]
- **`registers_t`** — the struct mirroring the exact stack layout the stubs build;
  the C handler's view of saved CPU state. [ch.3]
- **EOI (End Of Interrupt)** — the signal (`0x20`) sent to the PIC after handling an
  IRQ; without it the PIC stops delivering that line. [ch.3]
- **`iret`** — "interrupt return"; the instruction that resumes the interrupted code. [ch.3]
- **`cli` / `sti`** — clear / set the interrupt flag: disable / enable hardware
  interrupts. [ch.3]
- **`lgdt` / `lidt`** — instructions that load the GDT / IDT register. [ch.2, ch.3]
- **Handler registry** — the array of 256 function pointers MiniOS dispatches
  interrupts through via `register_interrupt_handler`. [ch.3]
- **Triple fault** — when a fault occurs while handling a fault while handling a
  fault; the CPU gives up and resets. The classic symptom of a broken IDT/stub. [ch.3]

## Drivers and I/O

- **Driver** — code that speaks one device's private protocol. [ch.4]
- **Port-mapped I/O** — talking to devices via the `in`/`out` instructions and 16-bit
  port numbers. [ch.4]
- **Memory-mapped I/O** — talking to devices by reading/writing fixed memory
  addresses (e.g. the VGA buffer at `0xB8000`). [ch.4]
- **Polling** — repeatedly asking a device for status instead of using interrupts;
  wasteful, avoided where possible. [ch.3, ch.4]
- **VGA text mode** — the 80×25, 2-bytes-per-cell text display at `0xB8000`. [ch.4]
- **Attribute byte** — the per-cell byte encoding foreground/background color
  (`0x0F` = white on black). [ch.4]
- **Scan code** — the raw hardware key number from the keyboard (not ASCII); bit 7
  set means key release. [ch.4]
- **PS/2** — the legacy keyboard/mouse interface; the keyboard reads scan codes from
  port `0x60` and raises IRQ 1. [ch.4]
- **PIT (Programmable Interval Timer, 8253/8254)** — the chip that fires IRQ 0 at a
  configurable frequency; base 1,193,180 Hz. [ch.4]
- **Divisor** — the value programmed into the PIT; it fires at `base / divisor`. [ch.4]
- **Tick** — one timer interrupt; MiniOS counts ticks as its notion of time. [ch.4]
- **`volatile`** — a C qualifier telling the compiler a variable may change outside
  normal flow (e.g. in an interrupt), so it must not cache it. [ch.4]
- **DMA (Direct Memory Access)** — hardware reading/writing RAM without the CPU
  copying each byte; used by real drivers, not MiniOS. [ch.4]

## Structure and process

- **Kernel** — the always-resident, fully-privileged core of the OS. MiniOS is
  all kernel. [ch.0]
- **User space** — normal, unprivileged programs; MiniOS has none yet. [ch.0]
- **System call** — the controlled doorway from user space into the kernel. [ch.0, ch.3]
- **Event loop** — the "initialise, then react to events forever" structure of a
  kernel; MiniOS's `while(1) hlt;`. [ch.6]
- **`hlt`** — the instruction that stops the CPU until the next interrupt; the basis
  of an idle-but-responsive kernel. [ch.6]
- **Busy-wait / spin** — looping without `hlt`, pinning the CPU at 100%. Avoided. [ch.6]
- **Command dispatcher** — the shell's `strcmp`-based command lookup. [ch.6]
- **Cooperative vs. preemptive** — whether tasks yield voluntarily or are
  interrupted by the timer. MiniOS is single-tasking; the timer enables the leap to
  preemptive multitasking. [ch.4, ch.6]
- **Scheduler** — the component that shares the CPU among tasks; not yet in MiniOS,
  but the timer interrupt is the hook for it. [ch.6]
- **`init` / PID 1** — the first user process a Unix kernel starts, which launches
  everything else. [ch.6]

## Toolchain and build

- **Freestanding** — a C environment with no standard library and no `main`
  assumptions; what kernels compile as. [ch.7]
- **Hosted** — a normal C environment with a full standard library and OS. [ch.7]
- **Cross-compiler** — a compiler that runs on one platform but targets another
  (`x86_64-elf-gcc`: runs on your Mac, targets bare-metal x86-64). [ch.7]
- **Target triple** — the `arch-vendor-os` string (e.g. `x86_64-elf`) naming what a
  compiler produces; `-elf` with no OS means "bare metal." [ch.7]
- **ELF (Executable and Linkable Format)** — the binary format the linker emits and
  QEMU's `-kernel` loads. [ch.7]
- **Linker script (`linker.ld`)** — the file specifying exactly where each section
  lands in the output binary (Multiboot header first, code at 1 MB). [ch.7]
- **Location counter (`.`)** — the linker's "current address" variable; `. = 1M`
  places the kernel at 1 MB. [ch.7]
- **Section (`.text`, `.rodata`, `.data`, `.bss`)** — named regions of a binary for
  code, constants, initialised data, and zeroed data respectively. [ch.7]
- **NASM** — the assembler MiniOS uses for its `.asm` files. [ch.7]
- **QEMU** — the emulator that runs `minios.bin` as a virtual x86-64 PC. [ch.7]
- **Object file (`.o`)** — compiled-but-not-yet-linked output from one source file. [ch.7]
