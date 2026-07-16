# Building, running, and debugging

This is the operational guide: what you need, how to build MiniOS, how to run it,
and how to debug it. It reflects what actually works on the development machine.

## Dependencies

MiniOS needs a cross toolchain that targets bare-metal x86-64 (not the host OS),
an assembler, and an emulator. The versions below are the ones actually in use on
the development machine (macOS on Apple Silicon, Homebrew):

| Tool | Version | Purpose |
|------|---------|---------|
| `x86_64-elf-gcc` | 16.1.0 | Freestanding C cross-compiler |
| `x86_64-elf-binutils` (`x86_64-elf-ld`) | 2.46.1 | Cross linker |
| `nasm` | 3.01 | Assembler (`-f elf64`) |
| `qemu-system-x86_64` | 11.0.0 | Emulator |

Why a cross-compiler? The host compiler produces binaries for the host OS on the
host CPU. A kernel needs code for a bare x86-64 machine with no OS underneath.
`x86_64-elf-gcc` produces exactly that, and the Makefile's freestanding flags
(`-ffreestanding -nostdlib -nodefaultlibs -fno-pie -mno-red-zone -mcmodel=kernel`)
keep it from pulling in host libc or startup code.

### Install (macOS, Homebrew)

```bash
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
```

### Install (Linux)

There is no distro package for an `x86_64-elf` cross compiler. On an x86-64 Linux
host the native `gcc`/`ld` can build the kernel because the host architecture
already matches the target and the freestanding flags prevent host libc/startup
code from being linked in. Install the native toolchain plus the assembler and
emulator:

```bash
sudo apt install build-essential nasm qemu-system-x86
```

Alternatively, build a dedicated `x86_64-elf` cross GCC from source per the OSDev
"GCC Cross-Compiler" guide (targeting `--target=x86_64-elf`). This takes 30-plus
minutes and is only needed if the native toolchain does not work for you. The
Makefile currently hard-codes `x86_64-elf-gcc`/`x86_64-elf-ld`; on Linux with the
native toolchain, override them (`make CC=gcc LD=ld`).

## Build

```bash
make
```

This assembles every `.asm` file with `nasm -f elf64`, compiles every `.c` file
with the cross-compiler, and links them with `linker.ld` into `minios.bin`, an
ELF image the Multiboot loader understands. To rebuild from scratch:

```bash
make clean && make
```

## Current expected failure

The build does **not** link yet, and this is expected. `make` will compile and
assemble everything cleanly, then fail at the link step with undefined references
to `isr0`-`isr31` and `irq0`-`irq15`:

```
x86_64-elf-ld: kernel/isr.o: in function `isr_install':
isr.c: undefined reference to `isr0'
...
isr.c: undefined reference to `irq15'
```

These symbols belong in `kernel/isr_stubs.asm`, which is still a stub. If you see
this, your setup is fine. See [README.md](README.md) for the full status. Until
those symbols exist, `minios.bin` is not produced and the run/debug steps below
cannot be exercised yet.

## Run

```bash
make run
```

This runs `qemu-system-x86_64 -kernel minios.bin`. On a working build a window
opens showing the banner and a shell prompt:

```
Welcome to MiniOS!
> _
```

Type a command and press Enter (Backspace works): `help`, `clear`, `hello`,
`tick`. To quit QEMU, close the window, or press `Ctrl-A` then `X` in the
launching terminal.

## Debug

### Triple faults

A misconfigured boot climb (see
[reference/boot-sequence.md](reference/boot-sequence.md)) causes a triple fault,
where the CPU resets instead of reporting an error. These flags make it visible:

```bash
qemu-system-x86_64 -kernel minios.bin -d int -no-reboot -no-shutdown
```

- `-d int` logs every interrupt and exception the CPU takes.
- `-no-reboot` stops QEMU from rebooting on triple fault, so the log survives.
- `-no-shutdown` keeps the VM around after the fault for inspection.

Likely triple-fault causes: a missing `PS` (huge) bit on a page-directory entry,
a page table that is not 4096-aligned, a table that was not zeroed, or an identity
map that does not cover the address of the code executing when `CR0.PG` is set.

### Source-level debugging with GDB

Start QEMU with a gdb stub, halted before the first instruction:

```bash
qemu-system-x86_64 -kernel minios.bin -s -S
```

- `-s` opens a gdb server on TCP port 1234.
- `-S` freezes the CPU at startup so you can set breakpoints before anything runs.

In another terminal:

```bash
gdb minios.bin
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

### Inspecting CPU state from the monitor

Route the QEMU monitor to the terminal and query the CPU directly:

```bash
qemu-system-x86_64 -kernel minios.bin -monitor stdio
```

Then at the `(qemu)` prompt, `info registers` dumps the control registers
(CR0/CR3/CR4), segment selectors, and RIP/RSP. This is the quickest way to
confirm long mode is active and paging is on without attaching a debugger.
