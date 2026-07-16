# Changelog

All notable changes to MiniOS are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). MiniOS has no releases
yet, so everything lives under Unreleased.

## [Unreleased]

### Added

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

### Removed

- `CONVERSION_NOTES.md` and `BOOT_NOTES.md`: ad-hoc notes whose content was
  absorbed into `docs/` (toolchain into `docs/building.md`, boot climb into
  `docs/reference/boot-sequence.md`, GDT/TSS into `docs/reference/gdt.md`), the
  ADRs, and this changelog.

### Pending (not yet implemented)

- `kernel/idt.c`: `idt_set_entry` gate packing and the `lidt` install are stubs.
- `kernel/isr_stubs.asm`: the `isr0`-`isr31` / `irq0`-`irq15` entry points and the
  common save/restore stubs are unwritten, so the kernel does not link yet.
- Several `learnings/` chapters still describe the 32-bit model and need rewriting
  for long mode.
