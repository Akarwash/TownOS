# 7. The Build System and Toolchain

**Source files:** `Makefile`, `linker.ld`, `libc/string.c`, `libc/mem.c`,
`include/types.h`

Writing OS code is only half the battle; the other half is *turning it into a
bootable binary*. Building a kernel is fundamentally different from building a
normal program, and understanding why demystifies a lot of "magic." This chapter
explains freestanding compilation, cross-compilers, linker scripts, and how raw
source becomes bytes a machine will execute at boot.

## The big idea: no ground beneath your feet

When you compile a normal C program, you stand on a mountain of invisible support:
a C standard library (`printf`, `malloc`), a runtime that runs before `main`, an
operating system that loads and launches you, and a CPU already in a comfortable
mode. **A kernel has none of that** — the kernel *is* the thing that provides those
services to others. So building a kernel means explicitly *removing* all the
assumptions a normal build makes.

Three things must be true for kernel code:

1. **No standard library** — there is no `libc` to link against; you *are* below it.
2. **No host assumptions** — the code must target the bare machine, not your
   development OS.
3. **Exact control over memory layout** — the binary must be arranged so the
   bootloader and CPU find things where they expect (the Multiboot header first,
   code at 1 MB).

Each corresponds to a piece of the MiniOS build.

## Freestanding vs. hosted C

The C standard defines two environments:

- A **hosted** environment (a normal app) assumes a full standard library and an
  OS. `main` is the entry point; the runtime sets up the stack, argv, etc.
- A **freestanding** environment assumes almost nothing — only a handful of headers
  that define types and limits. There is no `main`, no `printf`, no `malloc`.

A kernel is the definitive freestanding program. MiniOS's compiler flags make this
explicit:

```makefile
CFLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=kernel -fno-pie -nostdlib -nodefaultlibs -Wall -Wextra
```

- **`-ffreestanding`** — tells the compiler "no standard library, no `main`
  assumptions, don't emit calls into libc." Without it, the compiler might, say,
  replace a copy loop with a call to the `memcpy` that doesn't exist here.
- **`-m64`** — generate 64-bit code (long mode), even on a different host.
- **`-mno-red-zone`** — disable the 128-byte red zone below the stack pointer;
  it is unsafe once interrupts run in kernel mode.
- **`-mcmodel=kernel`** — assume the kernel lives in the top 2 GB of the address
  space, the memory model expected for a 64-bit kernel.
- **`-fno-pie`** — no position-independent code. A kernel loads at a *fixed*
  address (1 MB); PIE is for relocatable user programs and would add pointless
  indirection.
- **`-nostdlib -nodefaultlibs`** — do not link the standard library or default
  startup files. There is nothing to link them *to* on the bare machine.
- **`-Wall -Wextra`** — maximum warnings. In kernel code a warning is often a
  latent triple-fault; treat them as errors in spirit.

Because there is no libc, MiniOS **hand-writes its own** minimal one in `libc/`:
`strlen`, `strcmp`, `strcpy` (`string.c`) and `memcpy`, `memset` (`mem.c`). These
are the functions the kernel actually uses — the screen driver's `scroll` needs
`memcpy`, the shell needs `strcmp`. `include/types.h` similarly re-declares
`uint8_t`, `uint32_t`, `size_t`, and `NULL`, because even `<stdint.h>` and
`<stddef.h>` are technically part of the hosted world you have opted out of. You
are rebuilding the floor you normally stand on, one plank at a time.

## The cross-compiler: why `x86_64-elf-gcc`

Your Mac's built-in compiler produces binaries for *your Mac* — the host OS, the
host CPU (ARM), the host executable format (Mach-O). A kernel for bare-metal x86-64 needs
none of those. Compiling with the host toolchain would, at best, embed subtle
host assumptions and, at worst, silently produce something that cannot boot.

The solution is a **cross-compiler**: a compiler that runs on one platform (your
ARM Mac) but *produces code for a different target* (x86-64, bare metal, ELF
format). That target is spelled out in the name:

```
x86_64-elf-gcc
│      │   └── the compiler
│      └────── ELF output, no target OS ("bare metal")
└───────────── the x86-64 (64-bit x86) instruction set
```

The `-elf` with no OS name is the important part: it means "there is no operating
system on the target" — which is correct, because MiniOS *is* the operating system.
A compiler like `x86_64-linux-gnu-gcc` would assume Linux is present and produce code
that expects Linux system calls. `x86_64-elf-gcc` assumes nothing.

`nasm` (the assembler) and `qemu-system-x86_64` (the emulator) round out the
toolchain: NASM assembles `boot.asm`, `gdt_flush.asm`, and `isr_stubs.asm` into
object files, and QEMU emulates an x86-64 PC so you can run the result without
rebooting real hardware.

## The linker script: arranging the binary (`linker.ld`)

Compilation produces object files full of code and data, but *where* each piece
lands in the final binary matters intensely for a kernel. The bootloader scans the
first 8 KB for the Multiboot header; the CPU will execute starting at 1 MB. A
normal program lets the default linker script decide layout; a kernel must
**specify it exactly**. That is `linker.ld`:

```ld
ENTRY(_start)              /* the first instruction: boot.asm's _start (ch.1) */

SECTIONS {
    . = 1M;                /* place everything starting at physical 1 MB */

    .multiboot : { *(.multiboot) }   /* header FIRST, so the loader finds it */
    .text      : { *(.text) }        /* code */
    .rodata    : { *(.rodata) }      /* constants, string literals */
    .data      : { *(.data) }        /* initialised globals */
    .bss       : { *(COMMON) *(.bss) } /* zero-initialised globals + the stack */
}
```

Three decisions worth understanding:

- **`. = 1M`** sets the location counter to 1 MB. Below 1 MB on a PC lies a maze of
  legacy memory (the real-mode IVT, BIOS data, the VGA buffer at `0xB8000`,
  memory-mapped device regions). 1 MB is the first clean stretch of RAM, so kernels
  conventionally load there. The Multiboot header's flags asked the loader to honor
  this.
- **`.multiboot` first** guarantees the magic number sits within the first few KB
  where the loader scans (ch.1). Put it later and the loader would not recognise the
  kernel at all.
- **`.bss` last, with `COMMON`** collects zero-initialised data (including
  `boot.asm`'s 16 KB stack). `.bss` occupies *no space in the binary file* — the
  loader just zeroes that RAM — which is why an OS image is smaller than its runtime
  memory footprint.

The linker script is the bridge between "a pile of compiled functions" and "a
precisely-laid-out image the hardware will accept." It is the most kernel-specific
build artifact there is.

## The Makefile: orchestration (`Makefile`)

The Makefile just automates the toolchain into a repeatable pipeline:

```
each .c  --(x86_64-elf-gcc $(CFLAGS) -c)-->  .o
each .asm --(nasm -f elf64)------------->  .o
all .o   --(x86_64-elf-ld -T linker.ld)-->  minios.bin
minios.bin --(qemu-system-x86_64 -kernel)-> running OS
```

Key points:

- **List every source** in `C_SOURCES` and `ASM_SOURCES`. A classic bug (and the
  current state of MiniOS) is a source file existing on disk but missing from the
  Makefile — so it silently never compiles, and you get "undefined reference"
  errors at link time or, worse, stale behavior. If you add `keyboard.c`, you must
  add it here.
- **Pattern rules** (`%.o: %.c`) say "to make any `.o`, compile the matching `.c`."
  This avoids one rule per file.
- **Recipe lines must start with a real TAB**, not spaces — a make quirk that
  produces the baffling "missing separator" error. (Called out in the build spec.)
- **Output ELF, not raw binary.** QEMU's `-kernel` expects an ELF image and reads
  the Multiboot header from it. Passing `--oformat binary` to the linker strips the
  ELF structure and QEMU rejects it — another documented gotcha.

`make run` builds then launches QEMU; `make clean` deletes the `.o` files and
`minios.bin` so you can rebuild from scratch.

## The whole pipeline, start to finish

```
   boot.asm ─nasm─┐
gdt_flush.asm ─nasm┤
 isr_stubs.asm ─nasm┤
                    ├─► *.o ─┐
  kernel.c ─gcc─────┤        │
     gdt.c ─gcc─────┤        ├─ x86_64-elf-ld ─T linker.ld ─► minios.bin (ELF)
  screen.c ─gcc─────┤        │                                   │
    ... etc ─gcc────┘        │                          qemu -kernel minios.bin
                             │                                   │
                             └───────────────────────────────────► running MiniOS
```

Every arrow is a tool doing one job. Nothing here is magic once you see the flow:
freestanding compiler turns C into bare-metal object code, NASM turns assembly into
object code, the linker arranges it per your script into an ELF the Multiboot loader
understands, and QEMU boots it.

## Going further

- **`make` alternatives**: real kernels use recursive Make, Kbuild (Linux), CMake,
  or Meson, but the core pipeline — compile freestanding, link with a script — is
  identical.
- **Building your own cross-compiler**: `x86_64-elf-gcc` came from Homebrew, but the
  OSDev wiki's "GCC Cross-Compiler" guide walks through building binutils and GCC
  yourself. Doing it once teaches you what a "target triple" really means.
- **`objdump` and `readelf`** let you inspect `minios.bin`: `readelf -l` shows the
  segments and confirms the load address is 1 MB; `objdump -d` disassembles your
  kernel so you can see the actual machine code the compiler produced. Great for
  debugging a triple fault.

### Exercises

1. Why does removing `-ffreestanding` risk the compiler emitting a call to a
   `memcpy` that doesn't exist? What is the compiler allowed to assume without it?
2. `.bss` takes zero bytes in the file but 16 KB+ at runtime. Explain how that is
   possible and who zeroes it.
3. Add a hypothetical `net.c` driver. List every file you must edit for it to
   actually end up in `minios.bin`.
4. Run `readelf -l minios.bin` after a build. Which section appears first, and why
   does that matter for booting (ch.1)?
