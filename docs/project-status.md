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
- The IDT and the full interrupt path: 256-entry table, PIC remap to the
  self-describing vector map (hardware IRQs at 0x40-0x4F, every vector defined in
  `include/vectors.h`), all 48 entry points in `kernel/isr_stubs.asm`, and the C
  dispatch in `kernel/isr.c`. Exceptions are decoded into plain-English
  diagnostics (page-fault CR2 and error-code bits, GP-fault selector, double
  fault) rather than a bare vector number. See [reference/idt.md](reference/idt.md)
  and [decisions/0005-self-describing-vector-map.md](decisions/0005-self-describing-vector-map.md).
- The PIT timer on IRQ 0 (`kernel/timer.c`) and the PS/2 keyboard on IRQ 1
  (`drivers/keyboard.c`).
- VGA text output with scrolling and a cursor (`drivers/screen.c`).
- A bitmap physical frame allocator (`kernel/memory.c`).
- The interactive shell (`shell/shell.c`) with `help`, `clear`, `hello`, `tick`.
  (Present and working, but not on the current boot path — see below.)
- A minimal freestanding libc (`libc/string.c`, `libc/mem.c`).
- A drop to ring 3 (`kernel/usermode.c`, `user/user_program.c`): after init,
  `kernel_main` forges an `iretq` frame and runs a small program at CPL 3 in its
  own user-accessible pages (code at 4M, stack at 6-8M), while the kernel's own
  pages stay ring-0-only. The program executes a privileged instruction and the
  resulting #GP proves the drop is real. This activates the previously inert user
  GDT descriptors and `tss.rsp0`. See
  [reference/user-mode.md](reference/user-mode.md) and
  [decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).

The kernel builds, links into `minios.elf`, is repackaged as `minios.bin`, and
boots under QEMU. In the current build `kernel_main` hands off to ring 3 as its
last act (the ring-3 program faults and the kernel halts), so the interactive
shell — though compiled and working — is not reached. Swapping the
`enter_user_mode` call back for `shell_init` restores the shell.
See [building.md](building.md).

## What was never built

These are absent by design; MiniOS stops at a single-address-space kernel that
demonstrates a ring-3 drop but does not manage processes.

- **A way back from ring 3.** The kernel can enter ring 3, but the only path
  back into the kernel is a fault. There is no syscall entry: every IDT gate is
  DPL 0, so `int N` from ring 3 would itself fault, and no `syscall`/`sysret`
  MSRs are programmed. Ring-3 code cannot yet *request* anything of the kernel.
- **Processes.** The ring-3 drop runs one hard-coded program and never returns.
  There is no notion of a process, no loading, no exit, and no way to run a
  second user program.
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

**User mode.** Done. `kernel_main` drops to ring 3 and runs a program in its own
pages; the user GDT descriptors and `tss.rsp0` are now load-bearing. See
[decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).
The remaining steps build on it.

**System calls.** Ring-3 code runs but can only re-enter the kernel by faulting.
It needs a controlled doorway. Install one IDT gate at DPL 3 (or wire up the
`syscall`/`sysret` instructions and the associated MSRs) so user code can request
kernel services. The dispatch reuses the same registry pattern the interrupt
handlers already use: a call number selects a handler. This is the point where
the DPL-0-everywhere policy in the IDT gets its first deliberate exception.

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
- **The frame allocator is not usable in practice.** `memory_init()` now reserves
  the 4-8M frames the ring-3 program occupies (its code and stack), so the
  allocator no longer hands out memory that the running user program lives on.
  But that is the only fix: the first free frame is now at 8M, above the identity
  map, so `alloc_frame()` returns an address that page-faults on first touch,
  every time. Reserving the region fixes "do not hand out frames something else
  is using"; it does not fix "the frames handed out are not mapped." The pool
  stays unusable until the identity map is extended. See
  [reference/memory-map.md](reference/memory-map.md).
- **Memory sizes are invented, not measured.** Both the 8MB identity map
  (`boot/boot.asm`) and the 128MB frame pool (`kernel/memory.c`) are hardcoded
  numbers. Multiboot hands the kernel a memory map describing how much RAM the
  machine actually has, but `kernel_main` takes no arguments, so the Multiboot
  info pointer is discarded at boot and the map is ignored. The proper fix is to
  read that map and size both the identity map and the frame pool from it. This
  is recorded so it is not mistaken for a small oversight.
- **QEMU only.** The kernel has been built and booted under
  `qemu-system-x86_64`. It has not been run on real hardware or other emulators,
  and the `minios.bin` boot path relies on QEMU's built-in Multiboot `-kernel`
  loader.
