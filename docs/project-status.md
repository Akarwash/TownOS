# Project status

MiniOS is a learning kernel that reached its intended stopping point: it boots
x86-64 long mode and runs an interrupt-driven shell. This page records what works
today, what was deliberately never built, the natural next steps, and the known
limitations. It is a factual snapshot, not a roadmap.

## What works

- The 32 to 64 long-mode climb in `boot/boot.asm`: 2MB-page identity map of the
  first 8MB, PAE, EFER.LME, paging, a bootstrap GDT, and the far jump into 64-bit
  code that calls `kernel_main`.
- The kernel GDT and 64-bit TSS (`kernel/gdt.c`, `kernel/gdt_flush.asm`).
- The IDT and the full interrupt path: 256-entry table, PIC remap to vectors
  32-47, all 48 entry points in `kernel/isr_stubs.asm`, and the C dispatch in
  `kernel/isr.c`. See [reference/idt.md](reference/idt.md).
- The PIT timer on IRQ 0 (`kernel/timer.c`) and the PS/2 keyboard on IRQ 1
  (`drivers/keyboard.c`).
- VGA text output with scrolling and a cursor (`drivers/screen.c`).
- A bitmap physical frame allocator (`kernel/memory.c`).
- The interactive shell (`shell/shell.c`) with `help`, `clear`, `hello`, `tick`.
- A minimal freestanding libc (`libc/string.c`, `libc/mem.c`).

The kernel builds, links into `minios.elf`, is repackaged as `minios.bin`, and
boots to the shell under QEMU. See [building.md](building.md).

## What was never built

These are absent by design; MiniOS stops at a single-address-space, ring-0
kernel.

- **User mode.** Everything runs at ring 0. The GDT defines user code and data
  descriptors and the TSS carries an `rsp0` ring-0 stack pointer, but nothing
  ever enters ring 3, so both are inert scaffolding: present, correct, and
  unused.
- **System calls.** With no ring-3 code there is no syscall entry. No syscall
  vector is installed (every IDT gate is DPL 0, so `int N` from ring 3 is not a
  concern that arises).
- **A scheduler.** There is one thread of control: `kernel_main` and the shell.
  The timer counts ticks but never switches tasks.
- **Per-process paging.** Paging is on (it is required for long mode), but there
  is a single identity-mapped address space shared by everything. There are no
  per-process page tables and no address-space isolation.
- **A filesystem.** There is no block device, no disk driver, and no filesystem.
- **Program loading.** There is no ELF loader and no way to run a separate
  program; the shell dispatches to compiled-in command functions.

## Natural next steps

In dependency order. Each builds on the one before.

**User mode.** The first real step is entering ring 3. The GDT already has user
code and data descriptors and the TSS already holds a ring-0 stack, so the
groundwork is laid. What is missing is the transition itself: build a small
ring-3 stack frame and `iretq` into it with the user selectors, and confirm the
CPU switches to `rsp0` from the TSS when an interrupt fires in ring 3. Until this
works, none of the steps below have a reason to exist.

**System calls.** Once ring-3 code runs, it needs a controlled way into the
kernel. Install one IDT gate at DPL 3 (or wire up the `syscall`/`sysret`
instructions and the associated MSRs) so user code can request kernel services.
The dispatch reuses the same registry pattern the interrupt handlers already use:
a call number selects a handler. This is the point where the DPL-0-everywhere
policy in the IDT gets its first deliberate exception.

**A scheduler.** With more than one thread of control worth running, the timer
interrupt becomes a preemption point. Save the interrupted `registers_t`, pick
another task, and restore its saved frame. The machinery is already present: the
timer ticks and the ISR stubs already build a complete register frame on the
stack. A scheduler turns that tick into a context switch.

**Per-process paging.** Real isolation needs a separate address space per
process. Give each process its own top-level page table, switch `CR3` on context
switch, and handle the page fault (vector 14, which already has a gate and a stub)
to implement demand paging and to kill a process that touches memory it does not
own. This is the largest step and the one that turns MiniOS from a single-image
kernel into something that can safely run untrusted programs.

## Known limitations

- **No dynamic memory beyond the frame allocator.** `kernel/memory.c` hands out
  whole 4KB frames. There is no `kmalloc`/`kfree` heap for arbitrary-size
  objects, so kernel data structures are statically sized.
- **No SMP.** MiniOS assumes a single CPU. It uses the legacy 8259 PIC, not the
  APIC/IO-APIC, and has no per-core state or locking.
- **8MB identity map.** `boot/boot.asm` identity-maps only the first 8MB. Any
  physical address above 8MB is unmapped and would fault on access.
- **QEMU only.** The kernel has been built and booted under
  `qemu-system-x86_64`. It has not been run on real hardware or other emulators,
  and the `minios.bin` boot path relies on QEMU's built-in Multiboot `-kernel`
  loader.
