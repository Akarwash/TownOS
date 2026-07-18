# Project status

MiniOS is a learning kernel: it boots x86-64 long mode, drops to ring 3, and now
preempts between two ring-3 tasks on the timer tick. (An interrupt-driven shell is
still compiled and working but off the boot path.) This page records what works
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
- A bitmap physical frame allocator (`kernel/memory.c`), sized from the real RAM
  the Multiboot map reports (identity map extended to cover it, up to a 1GB cap),
  so it hands out real, mapped frames. See
  [reference/memory-map.md](reference/memory-map.md).
- The interactive shell (`shell/shell.c`) with `help`, `clear`, `hello`, `tick`.
  (Present and working, but not on the current boot path — see below.)
- A minimal freestanding libc (`libc/string.c`, `libc/mem.c`).
- A drop to ring 3 (`kernel/usermode.c`, `user/user_program.c`): after init,
  `kernel_main` forges an `iretq` frame and runs a small program at CPL 3 in its
  own user-accessible pages (code at 4M, stack at 6-8M), while the kernel's own
  pages stay ring-0-only. This activates the previously inert user GDT
  descriptors and `tss.rsp0`. See [reference/user-mode.md](reference/user-mode.md)
  and [decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).
- System calls (`kernel/syscall.c`, `include/syscalls.h`): the ring-3 programs
  call back into the kernel through one `int 0x50` gate, the only DPL 3 gate in
  the IDT. `SYS_WRITE` prints a string; `SYS_EXIT` halts. The dispatcher switches
  on RAX and returns its result in RAX; an unknown number is rejected, not fatal.
  The `SYS_WRITE` pointer check is a stopgap (see limitations below). See
  [reference/syscalls.md](reference/syscalls.md) and
  [decisions/0007-syscalls-via-int-0x50.md](decisions/0007-syscalls-via-int-0x50.md).
- A round-robin preemptive scheduler (`kernel/scheduler.c`): the timer tick
  switches between two ring-3 tasks by overwriting the interrupt frame on the
  kernel stack in place, so the stub's `iretq` resumes a different task.
  `task_create` forges a never-run task (the ring-3 drop generalised), a fixed
  `.bss` table holds them, and the two `user_program_a`/`_b` programs interleave
  "A" and "B" on screen forever. No address-space isolation and a fixed table of
  four (see limitations below). See [reference/scheduling.md](reference/scheduling.md)
  and [decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md).

The kernel builds, links into `minios.elf`, is repackaged as `minios.bin`, and
boots under QEMU. In the current build `kernel_main` hands off to the scheduler as
its last act (it creates two ring-3 tasks and enters task 0; the timer then
switches between them, and they print "A"/"B" forever), so the interactive shell,
though compiled and working, is not reached. Swapping the scheduler handoff back
for `shell_init` restores the shell. See [building.md](building.md).

## What was never built

These are absent by design; MiniOS stops at a single-address-space kernel that
preempts between two hard-coded ring-3 tasks but does not manage processes.

- **Processes.** The scheduler runs two hard-coded programs baked into the kernel
  image, not loaded programs. There is a `SYS_EXIT` syscall, but with no parent to
  return to it can only halt the machine. There is no notion of a process, no
  loading, and no way to add a third program without editing the kernel.
- **Per-process paging.** Paging is on (it is required for long mode), but there
  is a single identity-mapped address space shared by everything, tasks included.
  There are no per-process page tables and no address-space isolation, so one
  task can scribble on the other's stack (the two share one page, split in half).
- **A filesystem.** There is no block device, no disk driver, and no filesystem.
- **Program loading.** There is no ELF loader and no way to run a separate
  program; the shell dispatches to compiled-in command functions.

## Natural next steps

In dependency order. Each builds on the one before.

**User mode.** Done. `kernel_main` drops to ring 3 and runs a program in its own
pages; the user GDT descriptors and `tss.rsp0` are now load-bearing. See
[decisions/0006-user-mode-with-separate-pages.md](decisions/0006-user-mode-with-separate-pages.md).
The remaining steps build on it.

**System calls.** Done. Ring-3 code re-enters the kernel through one DPL 3 IDT
gate at `int 0x50` (`kernel/syscall.c`), the first and only deliberate exception
to the DPL-0-everywhere IDT policy. A call number in RAX selects a handler
(`SYS_WRITE`, `SYS_EXIT`). See
[decisions/0007-syscalls-via-int-0x50.md](decisions/0007-syscalls-via-int-0x50.md).
What remains for a real syscall layer is safe argument validation (see the
untrusted-pointer limitation below) and more calls, both of which wait on
processes and address spaces.

**A scheduler.** Done. The timer interrupt is now a preemption point: the tick
saves the interrupted `registers_t` into the current task's slot, picks the next
runnable task round-robin, and copies its saved frame back over the on-stack frame
in place, so the ISR stub's `iretq` resumes a different task. Two ring-3 tasks run
this way. See
[decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md).
What remains is everything isolation buys (below) and lifting the fixed
four-task, single-stack-page limits, both of which wait on an allocator and
address spaces.

**Per-process paging.** Real isolation needs a separate address space per
process. Give each process its own top-level page table, switch `CR3` on context
switch, and handle the page fault (vector 14, which already has a gate and a stub)
to implement demand paging and to kill a process that touches memory it does not
own. This is the largest step and the one that turns MiniOS from a single-image
kernel into something that can safely run untrusted programs. Today the two tasks
share one address space and one stack page (split by hand), so a stack overflow in
one silently corrupts the other.

## Known limitations

- **Syscall pointer validation is a stopgap, not real.** `SYS_WRITE` takes a
  pointer from ring 3, which is untrusted (the confused-deputy problem). The
  current check confirms only that the *start* pointer lies in the ring-3 region
  (`USER_REGION_START`..`USER_REGION_END`); it does **not** bound the string's
  length, so a string starting just below `USER_REGION_END` with no NUL still
  walks off the region into kernel pages. Real validation, checking the whole
  `[ptr, ptr+len)` range against the caller's mapped pages and capping the
  length, needs per-process address spaces that do not exist yet. Recorded as a
  TODO in `kernel/syscall.c`. Do not read the region check as real pointer
  safety. See [reference/syscalls.md](reference/syscalls.md).
- **No dynamic memory beyond the frame allocator.** `kernel/memory.c` hands out
  whole 4KB frames. There is no `kmalloc`/`kfree` heap for arbitrary-size
  objects, so kernel data structures are statically sized.
- **The scheduler is fixed and unisolated.** The task table is a fixed `.bss`
  array of four (`MAX_TASKS`), because there is no kernel heap to allocate tasks
  dynamically. The two tasks share a single 2MB user stack page split in half by
  hand, with no guard page between them, so a stack overflow in one corrupts the
  other. There is no per-task address space, no blocking or sleeping (a task
  yields only by being preempted), and no task exit that returns anywhere. See
  [reference/scheduling.md](reference/scheduling.md).
- **No SMP.** MiniOS assumes a single CPU. It uses the legacy 8259 PIC, not the
  APIC/IO-APIC, and has no per-core state or locking.
- **1GB identity-map ceiling.** The boot climb (`boot/boot.asm`) maps a fixed
  32MB, then `kernel/memory.c` reads the Multiboot map and extends the identity
  map to cover real RAM, capped at 1GB. The cap is real: the single `pd_table` is
  one 4KB page (512 x 2MB = 1GB), so physical RAM above 1GB is not mapped and is
  ignored. Lifting it needs more page directories, which is out of scope. The
  frame pool is sized from the same detected RAM, so `alloc_frame()` now returns
  real, mapped, writable memory (the old "invented sizes" and "allocator returns
  unmapped addresses" problems are gone). See
  [reference/memory-map.md](reference/memory-map.md) and
  [decisions/0009-read-multiboot-map-extend-identity-map.md](decisions/0009-read-multiboot-map-extend-identity-map.md).
- **QEMU only.** The kernel has been built and booted under
  `qemu-system-x86_64`. It has not been run on real hardware or other emulators,
  and the `minios.bin` boot path relies on QEMU's built-in Multiboot `-kernel`
  loader.
