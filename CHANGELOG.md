# Changelog

All notable changes to MiniOS are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Ring-3 (CPL 3) user mode. `kernel/usermode.c` (`enter_user_mode`) forges the
  five-value `iretq` frame (SS, RSP, RFLAGS, CS, RIP) and returns into ring 3
  with the user selectors (`GDT_SELECTOR_USER_CODE` 0x1B, `GDT_SELECTOR_USER_DATA`
  0x23) and IF kept set. `user/user_program.c` is a self-contained ring-3 program
  in a new `.user_text` section that writes its stack, then runs `cli` to force a
  #GP as proof it is really CPL 3. `kernel/kernel.c` drops to it after init. This
  activates the previously inert user GDT descriptors and `tss.rsp0`.
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
  `SYSCALL_VECTOR` (0x50, reserved, not wired up).
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

- `kernel_main` now hands off to ring 3 (`enter_user_mode`) as its last act
  instead of calling `shell_init`; the shell is still compiled and working but is
  off the boot path (the ring-3 program faults and the kernel halts before the
  idle loop). `docs/architecture.md`, `docs/project-status.md`, and
  `docs/reference/memory-map.md` updated for the ring-3 region (and its overlap
  with the frame allocator pool at 4M).
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
- Root `CHANGELOG.md` and `CONTRIBUTING.md`.

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
