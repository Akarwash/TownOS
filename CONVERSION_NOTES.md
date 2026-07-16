# MiniOS 32-bit → 64-bit conversion notes

This documents the **mechanical / portable** half of the x86-64 migration. The
privileged boot and interrupt internals were deliberately left as stubs so they
can be hand-written as a learning exercise.

> **The project does not build or boot yet.** The kernel intentionally will not
> link until the hand-written long-mode path (the 32→64 climb, 64-bit GDT/IDT
> install, and ISR save/restore) is filled in. See "Stubbed" below.

## Toolchain install (macOS, Homebrew — detected host)

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
```

For reference, on Debian/Ubuntu there is no distro `x86_64-elf` cross toolchain
package; build one from source per the OSDev "GCC Cross-Compiler" guide
(`--target=x86_64-elf`), and `sudo apt install nasm qemu-system-x86` for the
assembler and emulator. (Not run — documented only.)

## What was changed (mechanical / portable)

- **Makefile** — `CC=x86_64-elf-gcc`, `LD=x86_64-elf-ld`, `QEMU=qemu-system-x86_64`;
  CFLAGS now `-m64 -mno-red-zone -mcmodel=kernel` (kept `-ffreestanding -fno-pie
  -nostdlib -nodefaultlibs -Wall -Wextra`); ASMFLAGS `-f elf64`; LDFLAGS
  unchanged. Added a header comment with the cross-toolchain install commands.
  The source-file list logic is unchanged.
- **include/types.h** — added `uint64_t` / `int64_t` (`unsigned/signed long`);
  `size_t` is now `uint64_t`. Existing 8/16/32-bit types and `NULL` unchanged.
- **linker.ld** — added `OUTPUT_FORMAT(elf64-x86-64)` and
  `OUTPUT_ARCH(i386:x86-64)`. Kept `. = 1M` and the section layout; no
  higher-half remapping (that is part of the hand-written long-mode work).
- **kernel/memory.c / memory.h** — physical frame addresses widened from
  `uint32_t` to `uint64_t` (`alloc_frame` return, `free_frame` argument). Logic
  otherwise identical.
- **kernel/isr.h** — `registers_t` updated to 64-bit register names
  (`rdi, rsi, rbp, rbx, rdx, rcx, rax, r8–r15, int_no, err_code, rip, cs,
  rflags, user_rsp, ss`). Struct definition only — mechanical.
- **kernel/idt.h** — `idt_entry_t` updated to the 64-bit **16-byte** gate layout
  (offset split low/mid/high + `ist` + reserved `zero`); `idt_ptr_t.base` and
  the `idt_set_entry` base parameter widened to 64-bit. Layout only.
- **kernel/isr.c** — handler-address casts widened `(uint32_t)` → `(uint64_t)`
  to match `idt_set_entry`; gate-flags comment corrected to "64-bit interrupt
  gate". The vector table wiring is unchanged (it depends on the stubs below).
- **kernel/idt.c** — `idt_ptr.base` cast widened to `uint64_t`; PIC remap and
  IDT zeroing (portable) retained.
- Other portable C (`drivers/screen`, `drivers/ports`, `drivers/keyboard`,
  `libc/string`, `libc/mem`, `kernel/timer`, `shell/shell`, `kernel/kernel.c`)
  needed no source changes — no 32-bit-specific assumptions. `ports.c` inline
  `in`/`out` asm is identical on x86-64 and was left as-is.

## What was stubbed (HAND-WRITTEN — left as `TODO(long-mode)`)

These contain **no implementation**, only skeletons + `TODO(long-mode)` notes:

- **boot/boot.asm** — Multiboot header kept. The 32→64 climb (page tables, PAE,
  EFER.LME, enable paging, load 64-bit GDT, far-jump to 64-bit) and a real
  `_start` are stubbed; `_start` is a commented-incomplete placeholder.
- **kernel/gdt.c / gdt.h / gdt_flush.asm** — 64-bit GDT skeleton. Descriptor
  contents, `gdt_init` body, and `gdt_flush` are TODO. (The `gdt_ptr_t` 16-bit
  limit + 64-bit base pointer format — mechanical — is defined.)
- **kernel/idt.c** — `idt_set_entry` body (packing the address into the 16-byte
  gate) and the IDT install (`lidt`) are TODO. The gate **struct layout** is
  done (see above).
- **kernel/isr_stubs.asm** — all ISR/IRQ entry points and the common
  save/restore stubs are TODO. `pusha`/`popa` do not exist in 64-bit, so each
  GPR must be pushed/popped by hand in the order matching `registers_t`, and
  return is `iretq`. `isr.c` references these symbols, so the kernel will not
  link until they exist.

## Chapters that will need hand-written 64-bit updates

These `docs/` chapters are **conceptually** about the 32-bit / protected-mode
model, not just isolated wrong facts. Per the migration plan they were **not**
rewritten — the conceptual 64-bit versions are to be hand-written:

- **docs/01-how-an-os-boots.md** — currently ends the boot story in 32-bit
  protected mode; needs the 16→32→64 long-mode climb.
- **docs/02-protected-mode-and-the-gdt.md** — segmentation/GDT semantics differ
  in long mode (flat, base/limit largely ignored, the `L` bit, paging-based
  protection). Whole-chapter conceptual update.
- **docs/03-interrupts.md** — the register-save mechanism (`pusha`/`popa`,
  `registers_t`), the 8→16-byte gate, and the "32-bit interrupt gate" wording
  need the 64-bit interrupt mechanism. Left intact rather than half-edited.
- **docs/05-memory-management.md** — describes 32-bit x86 paging; long mode uses
  4-level (PML4) paging. Conceptual update.

Surgical mechanical fact corrections that *were* applied: the toolchain chapter
(`docs/07`), the `docs/glossary.md` toolchain/emulator entries, and the
`docs/README.md` summary (toolchain names, flags, `elf64`, `qemu-system-x86_64`,
"long mode"). `docs/lecture.md` and `learning.md` were not touched.

## Toolchain install + current build status (this machine)

Host detected: **macOS (Apple Silicon), Homebrew**. `nasm` and `qemu` were
already present; the cross gcc/binutils were installed:

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils
```

Verified versions:

- `x86_64-elf-gcc` — GCC 16.1.0
- `x86_64-elf-ld` — GNU binutils 2.46.1
- `nasm` — 3.01
- `qemu-system-x86_64` — 11.0.0

The cross compiler is available, so the Makefile's `CC`/`LD` were left unchanged
(no native-toolchain fallback needed); only the toolchain comment block was
refreshed to record what actually worked here.

`make` build status:

1. **C sources compile** — all 14 objects build cleanly with `x86_64-elf-gcc`,
   no warnings.
2. **`boot/boot.asm` assembles** — `nasm -f elf64`, clean. The 32→64 long-mode
   climb is now implemented (see `BOOT_NOTES.md`).
3. **Link still fails, as expected** — `x86_64-elf-ld` reports undefined
   references to the ISR/IRQ entry points that live in the still-stubbed
   `kernel/isr_stubs.asm` (referenced by `kernel/isr.c:isr_install`):
   `isr0`–`isr31` and `irq0`–`irq15` (48 symbols). Filling in
   `isr_stubs.asm` (and the `gdt.c`/`gdt_flush.asm`/`idt.c` stubs) is the
   remaining hand-written work.

QEMU was not run.

## Not done, on purpose

- No QEMU run was attempted.
- No new teaching/conceptual docs about long mode, paging, the boot climb, or
  the 64-bit interrupt/GDT mechanism were written.
