# Changelog

All notable changes to MiniOS are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Reading the real amount of RAM from the Multiboot memory map and extending the
  identity map to cover it. `boot/boot.asm` now identity-maps a fixed 32MB (up
  from 8MB, 16 2MB PD entries; 4-8M stays `PG_USER`, the rest kernel-only),
  exposes `pd_table` as `global`, and forwards the Multiboot info pointer (left in
  EBX by the bootloader) to `kernel_main` in RDI. `kernel_main` now takes
  `uint64_t multiboot_info_addr`. `kernel/multiboot.h` defines the Multiboot 1
  info and mmap-entry structures (packed; the entry stride is
  `size + sizeof(size)`, since `size` does not count itself). `kernel/memory.c`
  (`memory_detect_and_map`) walks the map for the highest usable physical address,
  fills `pd_table` from 32M up to that address (rounded to 2MB, capped at 1GB, the
  single PD page's reach), and reloads CR3 to flush the TLB. The frame pool is
  sized from the same measured RAM, and every non-usable range the map reports is
  reserved by walking the map, so `alloc_frame()` now returns real, mapped,
  writable memory. This retires the invented 8MB map and 128MB pool constants and
  the "allocator returns unmapped addresses" limitation. If the map is absent
  (flags bit 6 clear), the kernel falls back to the fixed 32MB and warns rather
  than reading garbage. The boot banner prints the detected RAM. See
  `docs/decisions/0009-read-multiboot-map-extend-identity-map.md` and
  `docs/reference/memory-map.md`.
- A round-robin preemptive scheduler (`kernel/scheduler.c`, `kernel/scheduler.h`).
  The timer IRQ handler now switches between ring-3 tasks by overwriting the
  interrupt frame on the kernel stack in place: the `registers_t` the stub pushed
  is the task's whole context, so `schedule()` saves `*r` into the current task's
  slot, picks the next `TASK_READY` task round-robin, and copies its saved frame
  back over `*r`, so the stub's `iretq` resumes a different task. `task_create`
  forges a never-run task's frame (the `enter_user_mode` trick generalised: `rip`
  = entry, `user_rsp` = stack top, `cs`/`ss` = user selectors, `rflags` = 0x202
  with IF set, GPRs zero) and marks it `TASK_READY`. Tasks live in a fixed `.bss`
  array `tasks[MAX_TASKS]` (no kernel heap yet). `scheduler_start()` enters task 0
  by reusing `enter_user_mode`; a `scheduler_running` guard plus a `cli` in
  `scheduler_start` close the startup race where early timer ticks fire in kernel
  context. EOI stays before the switch (already sent by `irq_handler`), or the
  timer line would freeze after one switch.
- A second ring-3 program in `user/user_program.c`: `user_program_a` and
  `user_program_b` each loop forever calling `SYS_WRITE` with "A"/"B" and a crude
  busy-wait delay, neither calling `SYS_EXIT`, so the scheduler's switching stays
  visible. `SYS_EXIT` stays implemented but unused.
- `TASK0_STACK_TOP` (0x700000) / `TASK1_STACK_TOP` (0x800000) in
  `kernel/scheduler.h`: the single 6-8M PG_USER stack page (PD[3]) split in half,
  one 1MB stack per task (no guard page between them).
- `docs/decisions/0008-round-robin-preemptive-scheduler.md` and
  `docs/reference/scheduling.md` documenting the frame-swap switch, forging a
  never-run task, the two traps (overwrite in place, EOI before switch), task
  states, the startup race, and the stack split.
- System calls through a single `int 0x50` gate. `kernel/isr.c` installs one
  DPL 3 IDT gate at `SYSCALL_VECTOR` (0x50) with a new `GATE_USER` (0xEE) flags
  byte; every other gate stays DPL 0, so this is the only vector ring-3 code can
  raise on purpose. `kernel/isr_stubs.asm` adds `syscall_stub` (mirrors
  `ISR_NOERR`: dummy error code + vector, then a shared tail that calls the C
  dispatcher). `kernel/syscall.c` (`syscall_handler`) switches on RAX and returns
  its result in RAX: `SYS_WRITE` prints a NUL-terminated string, `SYS_EXIT` halts;
  an unknown number is reported and rejected with `-1` rather than faulting. The
  ABI numbers live in a deliberately standalone `include/syscalls.h` (numbers
  only, no kernel code) so a ring-3 program can compile against them.
- `.user_rodata` section in `linker.ld`, placed in the `:user` `PT_LOAD` segment
  (4-8M) so the ring-3 program's string literals land in user-accessible pages
  instead of `.rodata` at 1M (kernel pages).
- `docs/decisions/0007-syscalls-via-int-0x50.md` and `docs/reference/syscalls.md`
  documenting the gate, the calling convention, the two calls, and the stopgap
  pointer check.
- Ring-3 (CPL 3) user mode. `kernel/usermode.c` (`enter_user_mode`) forges the
  five-value `iretq` frame (SS, RSP, RFLAGS, CS, RIP) and returns into ring 3
  with the user selectors (`GDT_SELECTOR_USER_CODE` 0x1B, `GDT_SELECTOR_USER_DATA`
  0x23) and IF kept set. `user/user_program.c` is a self-contained ring-3 program
  in a new `.user_text` section (it calls the kernel through `int 0x50`, see the
  syscall entry above). `kernel/kernel.c` drops to it after init. This activates
  the previously inert user GDT descriptors and `tss.rsp0`.
- `GDT_SELECTOR_*` constants in `kernel/gdt.h` (kernel/user code/data, TSS) as the
  single source of truth for selector values; `kernel/gdt.c` loads the TSS by name.
- `PG_USER` (page user/US bit) in `boot/boot.asm`. `PML4[0]`/`PDPT[0]` are made
  permissive (user bit set) and the PD leaves gate real access: `PD[0]`/`PD[1]`
  stay kernel-only, `PD[2]` (4-6M, user code) and `PD[3]` (6-8M, user stack) get
  the user bit. The PD loop is unrolled into four explicit per-region writes.
- `.user_text` section in `linker.ld` at `0x400000`, placed in its own `PT_LOAD`
  segment via a new `PHDRS` block so the 1M-to-4M gap is not padded into the file
  (`minios.bin` stays ~25KB).
- `docs/decisions/0006-user-mode-with-separate-pages.md` and
  `docs/reference/user-mode.md` documenting the ring-3 drop, the page-privilege
  layout, and how the #GP / #PF results prove it.
- `include/vectors.h`, the single source of truth for every interrupt vector
  number: all 32 named CPU exceptions, `PIC_MASTER_VECTOR_BASE` (0x40) and
  `PIC_SLAVE_VECTOR_BASE` (0x48), every IRQ vector derived from the base
  (`IRQ_TIMER` = 0x40, `IRQ_KEYBOARD` = 0x41, through `IRQ_15`), and
  `SYSCALL_VECTOR` (0x50, now the live syscall gate).
- Diagnostic exception handlers in `kernel/isr.c`: `isr_handler` now decodes the
  fault instead of printing a bare number. Page faults print the faulting CR2
  address and decoded error-code bits (read/write, present, user/kernel,
  reserved-bit, instruction fetch); general protection faults decode the
  offending selector or state plainly that the error code is zero; double faults
  explain that the error code carries no information. Every exception prints its
  vector, name, and RIP/CS/RFLAGS, then halts with `cli; hlt`.
- `docs/decisions/0005-self-describing-vector-map.md` recording the vector-map
  decision and its costs.

### Changed

- `kernel_main` now creates two ring-3 tasks and starts the scheduler
  (`scheduler_start`) as its last act instead of calling `enter_user_mode`
  directly; the tasks run forever, so the machine no longer halts on `SYS_EXIT`.
  `kernel/timer.c`'s `timer_callback` now calls `schedule(regs)` on every tick
  instead of only counting. `enter_user_mode` is still used, now by
  `scheduler_start` to enter task 0. `docs/architecture.md`,
  `docs/project-status.md`, and `docs/reference/memory-map.md` updated for the two
  tasks and the split stack page.
- `memory_init` now reserves the frames covering the ring-3 region (4-8M,
  `USER_REGION_START`/`USER_REGION_END`) so the frame allocator never hands out
  memory the running user program's code or stack already occupy. This resolves
  the pool/ring-3 overlap but does not make the allocator usable: the first free
  frame is now at 8M, above the 8M identity map, so `alloc_frame()` returns an
  address that page-faults on first touch until the map is extended. Both the 8M
  identity map and the 128M pool size remain invented numbers; sizing them from
  the (currently discarded) Multiboot memory map is recorded as future work.
  `docs/reference/memory-map.md` and `docs/project-status.md` updated.
- `kernel_main` now hands off to ring 3 (`enter_user_mode`) as its last act
  instead of calling `shell_init`; the shell is still compiled and working but is
  off the boot path (the ring-3 program prints via `SYS_WRITE` and then `SYS_EXIT`
  halts the machine before the idle loop). `docs/architecture.md`,
  `docs/project-status.md`, and `docs/reference/memory-map.md` updated for the
  ring-3 region (and its overlap with the frame allocator pool at 4M).
- Moved hardware IRQs off the conventional 0x20 base to a self-describing map:
  CPU exceptions at 0x00-0x1F, hardware IRQs at 0x40-0x4F, syscalls reserved at
  0x50-0x5F, so the high nibble names the category in a fault log. `pic_remap()`
  in `kernel/idt.c` now programs ICW2 to 0x40/0x48; `kernel/isr.c` installs the
  IRQ gates off the base; `kernel/isr_stubs.asm` derives IRQ vectors from a
  `PIC_MASTER_VECTOR_BASE` `equ` kept in sync with `vectors.h` by hand.
- Fixed the End-Of-Interrupt slave-PIC check in `kernel/isr.c` to compare against
  `PIC_SLAVE_VECTOR_BASE` instead of a hardcoded 40, which would otherwise stop
  acknowledging IRQ 8 to 15 (now at 0x48-0x4F).
- `kernel/timer.c` and `drivers/keyboard.c` register against `IRQ_TIMER` /
  `IRQ_KEYBOARD` from `vectors.h`, dropping their local `IRQ0_INTERRUPT` /
  `IRQ1_INTERRUPT` defines.
- `docs/reference/idt.md` updated for the new vector map and the `vectors.h`
  pointer; `learnings/03-interrupts.md` carries a note that its 32-47 numbering
  is superseded, pointing at ADR 0005.

## [0.1.0] - 2026-07-17

First booting version. MiniOS builds, links, and boots to an interactive shell
under QEMU, with the timer and keyboard driving interrupts.

### Added

- 64-bit IDT install in `kernel/idt.c`: `idt_set_entry` packs a 64-bit handler
  address across `offset_low`/`offset_mid`/`offset_high`, sets selector `0x08`,
  IST 0, and gate flags `0x8E` (present, DPL 0, interrupt gate), and `idt_init`
  loads the register with `lidt`.
- Interrupt entry points in `kernel/isr_stubs.asm`: `isr0`-`isr31` and
  `irq0`-`irq15`, with the `ISR_NOERR`/`ISR_ERR` macros normalising the error-code
  frame (real error code on vectors 8, 10, 11, 12, 13, 14, 17; dummy 0 elsewhere)
  and the common stubs that save the 15 general-purpose registers, pass a
  `registers_t*` in `RDI`, call the C dispatcher, and return with `iretq`.
- Boot path fix in the `Makefile`: link to `minios.elf` (ELF64, keeps gdb
  symbols), then repackage with `x86_64-elf-objcopy -O elf32-i386` into
  `minios.bin`, which QEMU's Multiboot `-kernel` loader accepts.
- `docs/reference/idt.md` documenting the IDT and interrupt entry path, and
  `docs/project-status.md` recording what works, what was never built, and the
  next steps.
- 32 to 64 long-mode climb in `boot/boot.asm`: three-level 2MB-page identity map
  of the first 8MB, PAE, `CR3`, EFER.LME, paging enable, a bootstrap GDT, and the
  far jump into 64-bit code that calls `kernel_main`.
- Kernel GDT and 64-bit TSS in `kernel/gdt.c`, `kernel/gdt.h`, and
  `kernel/gdt_flush.asm`: a 7-slot GDT (null, kernel code/data, user code/data,
  16-byte TSS descriptor), a dedicated ring-0 stack, and the `retfq` CS reload.
- x86-64 cross toolchain installed and pinned in the build docs
  (`x86_64-elf-gcc` 16.1.0, `x86_64-elf-binutils` 2.46.1, `nasm` 3.01,
  `qemu-system-x86_64` 11.0.0).
- Project documentation under `docs/`: architecture, building, reference pages
  (boot sequence, memory map, GDT/TSS), and architecture decision records.
- Root `CHANGELOG.md`.

### Changed

- Migrated the codebase from 32-bit i686 to x86-64: `Makefile` toolchain and
  flags (`-m64 -mno-red-zone -mcmodel=kernel`, `nasm -f elf64`), `include/types.h`
  (added 64-bit types, `size_t` widened), `linker.ld` (`elf64-x86-64` output),
  `kernel/memory.c` frame addresses widened to 64-bit, and the `registers_t` /
  `idt_entry_t` layouts updated to their 64-bit forms.
- Reorganized documentation: the former `docs/` teaching chapters and `learning.md`
  moved to `learnings/` (conceptual material), and `docs/` now holds factual
  project documentation. The two are kept strictly separate.
- Root `README.md` rewritten as a short entry point that signposts `docs/` and
  `learnings/` instead of embedding the full guide.
- `learnings/` chapters 1, 2, 3, and 5 carry a note that they describe the
  original 32-bit design, each linking to the current x86-64 reference page; the
  index states that `docs/` is the current source of truth.

### Removed

- `CONVERSION_NOTES.md` and `BOOT_NOTES.md`: ad-hoc notes whose content was
  absorbed into `docs/` (toolchain into `docs/building.md`, boot climb into
  `docs/reference/boot-sequence.md`, GDT/TSS into `docs/reference/gdt.md`), the
  ADRs, and this changelog.
