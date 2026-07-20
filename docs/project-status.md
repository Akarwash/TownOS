# Project status

MiniOS is a learning kernel: it boots x86-64 long mode, drops to ring 3, and
preempts between three ring-3 tasks on the timer tick, each in its own address
space (per-process paging). (An interrupt-driven shell is still compiled and
working but off the boot path.) This page records what works today, what was
deliberately never built, the natural next steps, and the known limitations. It
is a factual snapshot, not a roadmap.

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
- A polled ATA PIO disk driver (`drivers/disk.c`): `disk_read` and `disk_write`
  move any run of contiguous 512-byte blocks between a disk and a buffer on the
  primary ATA bus, addressed by LBA28. It polls the status port (no interrupts,
  no DMA) with bounded poll loops that time out rather than hang, and sets nIEN so
  the drive never raises IRQ14. `make run` attaches a 16MB raw `disk.img`. This is
  a raw block device, not a filesystem: it moves the exact block it is told to and
  has no names, files, or free-space tracking. A transfer freezes the machine (the
  scheduler cannot preempt mid-transfer), an accepted limitation of polled PIO. See
  [reference/disk.md](reference/disk.md) and
  [decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
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
  switches between several ring-3 tasks (three today) by overwriting the interrupt
  frame on the kernel stack in place and loading the next task's CR3, so the
  stub's `iretq` resumes a different task in its own address space. `task_create`
  `kmalloc`s a `task_t`, builds its private address space, and forges it as a
  never-run task (the ring-3 drop generalised); a pointer array tracks the
  heap-allocated tasks. The three `user_program_a`/`_b`/`_c` programs interleave
  "A", "B", and "C" on screen forever. The old fixed four-task ceiling and the
  shared user-stack region are both gone. See
  [reference/scheduling.md](reference/scheduling.md),
  [decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md),
  and [decisions/0011-dynamic-tasks-and-stacks.md](decisions/0011-dynamic-tasks-and-stacks.md).
- Per-process paging (`kernel/paging.c`): each task has its own page-table tree,
  loaded into CR3 on every context switch, so two tasks use the same virtual
  addresses (code `0x400000`, stack top `0x800000`) backed by different physical
  frames. Each tree has a private 4KB user half (a per-task copy of the ring-3
  image and a fresh stack) and a kernel half cloned from the boot tables by value,
  kernel-only, so the kernel is mapped in every tree (interrupts land in mapped
  kernel code without a CR3 change). This is real address-space isolation: a stray
  pointer faults instead of corrupting a neighbour. The by-value kernel clone rests
  on kernel mappings being frozen after boot (a documented tripwire). See
  [reference/paging.md](reference/paging.md) and
  [decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md).
- A kernel heap (`kernel/heap.c`), `kmalloc`/`kfree`: an explicit free list with
  boundary tags and coalescing, ported from the CMSC216 p5 `el_malloc`. It draws
  its slab from `alloc_frames_contiguous` (a new multi-page frame helper in
  `kernel/memory.c`), grows on demand, and guards its critical section with a
  save-and-restore interrupt disable so the timer IRQ cannot corrupt the free
  list mid-relink. This is the layer that would implement `mmap`. It now has real
  callers on the boot path: the heap-allocated `task_t` structs and the
  per-process `address_space_t` handles. See
  [reference/heap.md](reference/heap.md) and
  [decisions/0010-kernel-heap-ported-from-p5.md](decisions/0010-kernel-heap-ported-from-p5.md).

The kernel builds, links into `minios.elf`, is repackaged as `minios.bin`, and
boots under QEMU. In the current build `kernel_main` hands off to the scheduler as
its last act (it creates three ring-3 tasks, each in its own address space, and
enters task 0; the timer then switches between them, and they print "A"/"B"/"C"
forever), so the interactive shell, though compiled and working, is not reached.
Swapping the scheduler handoff back for `shell_init` restores the shell. See
[building.md](building.md).

## What was never built

These are absent by design; MiniOS isolates and preempts between hard-coded
ring-3 tasks in their own address spaces, but does not load or manage processes.
There is now a block device (the ATA disk driver), but nothing is built on it
yet.

- **Processes.** The scheduler runs hard-coded programs baked into the kernel
  image (three today), not loaded programs. Each task now has its own address
  space (per-process paging), but the program it runs is still compiled in: there
  is a `SYS_EXIT` syscall, but with no parent to return to it can only halt the
  machine, and there is no loading and no way to add a program without editing the
  kernel (though tasks are created dynamically at runtime rather than from a fixed
  table).
- **Demand paging, copy-on-write, and swap.** Per-process paging exists, but every
  page is mapped eagerly at `task_create` and backed by real frames. There is no
  lazy allocation on fault, no copy-on-write sharing (the read-only user text is
  copied in full per task rather than shared, `TODO(shared-text)`), and no paging
  to disk.
- **A filesystem.** There is a block device now (the polled ATA disk driver,
  `drivers/disk.c`), but nothing on top of it: no on-disk layout, no names, no
  files or directories, and no free-space tracking. The driver moves the exact
  512-byte block it is told to; deciding which block holds what is the filesystem
  layer, still absent. See [reference/disk.md](reference/disk.md).
- **Program loading.** There is no ELF loader and no way to run a separate
  program; the shell dispatches to compiled-in command functions. This waits on a
  filesystem to load a program image from.

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
in place, so the ISR stub's `iretq` resumes a different task. Three ring-3 tasks
run this way. The task structs are now heap-allocated and their stacks
bump-allocated from the user region, so the fixed four-task ceiling is gone (see
[decisions/0011-dynamic-tasks-and-stacks.md](decisions/0011-dynamic-tasks-and-stacks.md)).
See also
[decisions/0008-round-robin-preemptive-scheduler.md](decisions/0008-round-robin-preemptive-scheduler.md).
What remains is everything isolation buys (below) and lifting the single-stack-
region limit, which waits on per-process address spaces.

**Per-process paging.** Done. Each task has its own page-table tree, loaded into
CR3 on every context switch (`kernel/paging.c`): a private 4KB user half on its
own frames and a kernel half cloned from the boot tables by value. Two tasks use
the same virtual addresses backed by different physical memory, so a stray or
overflowing pointer faults instead of corrupting a neighbour. See
[decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md).
What remains builds on it: handling the page fault (vector 14, which already has
a gate and a stub) for demand paging and to kill a process that touches memory it
does not own, copy-on-write to share the read-only text instead of copying it per
task, and finally loaded processes rather than compiled-in programs.

**A block device.** Done. `drivers/disk.c` is a polled ATA PIO driver that reads
and writes 512-byte blocks by LBA on the primary bus. It is the raw storage layer
a filesystem needs. See
[decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
The remaining steps build on it.

**A filesystem, then program loading.** Next. With a block device in place, a
filesystem can turn names into block numbers and track free space, and a program
loader can then read an ELF image off disk into a fresh address space and run it
as a real process. Both are still absent.

## Known limitations

- **Syscall pointer validation is a stopgap, not real.** `SYS_WRITE` takes a
  pointer from ring 3, which is untrusted (the confused-deputy problem). The
  current check confirms only that the *start* pointer lies in the ring-3 region
  (`USER_REGION_START`..`USER_REGION_END`); it does **not** bound the string's
  length, so a string starting just below `USER_REGION_END` with no NUL still
  walks off the region into kernel pages. Real validation, checking the whole
  `[ptr, ptr+len)` range against the caller's mapped pages and capping the
  length, is now possible (each task has a private tree that can be walked to
  confirm a page is mapped and user-accessible) but is not yet implemented: the
  check still only tests the start pointer against the fixed region constants.
  Recorded as a TODO in `kernel/syscall.c`. Do not read the region check as real
  pointer safety. See [reference/syscalls.md](reference/syscalls.md).
- **The scheduler still has no blocking, sleeping, or task exit.** Task structs
  are heap-allocated and each task now has its own address space with a private
  stack (per-process paging), so the old fixed four-task ceiling and the shared
  user-stack region are both gone: a stack overflow faults in the offending task
  instead of corrupting a neighbour. What remains: there is no blocking or
  sleeping (a task yields only by being preempted), no task exit that returns
  anywhere (`SYS_EXIT` halts the machine), and no reclamation (a task's frames and
  tree are never freed, since tasks are never destroyed). Memory is also used
  wastefully: the read-only user text is copied in full per task rather than
  shared (`TODO(shared-text)`). See
  [reference/scheduling.md](reference/scheduling.md),
  [reference/paging.md](reference/paging.md), and
  [decisions/0012-per-process-paging.md](decisions/0012-per-process-paging.md).
- **Disk transfers freeze the machine.** The disk driver polls, so the CPU spins
  in the wait loops for the whole transfer and nothing else runs, including the
  scheduler: the timer tick cannot preempt a task while a block is moving. This is
  slow and blocking, an accepted limitation of polled PIO. The fix is
  interrupt-driven transfer (IRQ14 when a block is ready) and then DMA, recorded as
  future work in
  [decisions/0013-ata-pio-disk-driver.md](decisions/0013-ata-pio-disk-driver.md).
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
