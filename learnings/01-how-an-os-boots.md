# 1. How an OS Boots

**Source files:** `boot/boot.asm`, `linker.ld`, `kernel/kernel.c`

> **Note: this chapter describes the original 32-bit design.** MiniOS is now an
> x86-64 long-mode kernel. The concepts below still hold, but some specifics no
> longer match the source: in particular the `_start` shown here predates the
> 32-to-64 long-mode climb that `boot/boot.asm` now performs before calling
> `kernel_main`. For what actually runs today, see
> [../docs/reference/boot-sequence.md](../docs/reference/boot-sequence.md). This
> chapter is kept as personal learning material and is intentionally not rewritten.

The boot process is the answer to a deceptively hard question: *when the CPU has
literally nothing loaded, how does the first useful instruction get to run?*

## The big idea

Booting is a **bootstrap**: a series of progressively more capable programs, each
one loading the next. You cannot load a 5 MB kernel with nothing; but you *can*
run a tiny fixed program, which loads a slightly bigger one, which loads yours.
The name comes from "pulling yourself up by your bootstraps" — and yes, that is
also where "boot" and "reboot" come from.

A typical PC chain looks like this:

```
power on
  → CPU starts executing firmware (BIOS/UEFI) from a fixed address
    → firmware finds a boot device and loads a bootloader (GRUB, etc.)
      → bootloader loads the kernel into memory and jumps to it
        → kernel takes over forever
```

Each arrow is a handoff. Each stage exists because the one before it was too
dumb or too small to do the next job directly.

## How the hardware forces this

When an x86 CPU powers on, it is deliberately primitive for backwards
compatibility with 1978:

- It runs in **16-bit real mode** — it behaves like an original Intel 8086. It can
  only address ~1 MB of RAM, has no memory protection, and any program can do
  anything.
- It begins executing at a fixed physical address baked into the silicon, where
  the motherboard maps the firmware ROM.

So the firmware is the only thing that *can* run first, because the CPU is
hard-wired to jump to it. The firmware's job is to do basic hardware setup and
then find something to boot. It is not your kernel's friend — its job is just to
get *some* code off the disk and run it.

### Where GRUB and Multiboot come in

Writing a bootloader that talks to disks, parses filesystems, and switches CPU
modes is a whole project by itself. To avoid that, MiniOS uses the **Multiboot
Specification**. Multiboot is a *contract* between a bootloader (like GRUB) and a
kernel:

> "If your kernel begins with a special header containing a magic number, I (the
> bootloader) will load you into memory, put the CPU into 32-bit protected mode,
> and jump to you with a known register state."

That contract is enormously valuable. It means MiniOS does **not** have to write
16-bit real-mode assembly, does **not** have to switch CPU modes by hand, and does
**not** have to touch disk controllers. QEMU's `-kernel` flag contains a built-in
Multiboot loader, so QEMU itself plays the role of GRUB.

## How MiniOS does it

Open `boot/boot.asm`. It has three parts, each a section the linker will place in
the final binary.

### 1. The Multiboot header

```asm
MBOOT_MAGIC      equ 0x1BADB002
MBOOT_PAGE_ALIGN equ 1 << 0
MBOOT_MEM_INFO   equ 1 << 1
MBOOT_FLAGS      equ MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO
MBOOT_CHECKSUM   equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
    align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM
```

- `0x1BADB002` is the magic number the loader scans for in the first 8 KB of the
  kernel. It literally reads "1 BAD B002" — a joke, but a load-bearing one.
- The **flags** request services: bit 0 asks the loader to page-align loaded
  modules; bit 1 asks it to hand us a memory map.
- The **checksum** is chosen so that `magic + flags + checksum == 0` (mod 2^32).
  The loader verifies this to be confident it found a real header and not a random
  `0x1BADB002` in your data. This "the fields must sum to zero" trick shows up
  again in the GDT and IDT — it is a cheap integrity check.

The header must appear near the very start of the binary, which is why
`linker.ld` places the `.multiboot` section *first* (chapter 7).

### 2. The stack

```asm
section .bss
    align 16
stack_bottom:
    resb 16384        ; reserve 16 KB, uninitialised
stack_top:
```

C code cannot run without a stack — function calls, local variables, and saved
registers all live there. The firmware/loader does not guarantee a usable stack,
so MiniOS reserves its own 16 KB in the `.bss` section. `.bss` is "uninitialised
data": it takes zero space in the binary file and is simply zeroed in RAM at load
time, which is why we use `resb` (reserve bytes) instead of writing out 16 KB of
zeros.

Note the label trick: the stack grows **downward** on x86, so `stack_top` (the
higher address) is where `esp` should start, and it grows toward `stack_bottom`.

### 3. The entry point

```asm
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top   ; give C a stack
    call kernel_main     ; jump into C
    cli                  ; if kernel_main ever returns, disable interrupts
    hlt                  ; and halt the CPU forever
```

This is the seam between assembly and C. `_start` is the symbol the linker marks
as the entry point (`ENTRY(_start)` in `linker.ld`). It does the one thing C
cannot do for itself — set up the stack pointer — then `call kernel_main`.

The `cli; hlt` after the call is a safety net. `kernel_main` is not *supposed* to
return; but if it ever did, we do not want the CPU to wander off executing
whatever bytes come next in memory. `cli` disables interrupts and `hlt` stops the
CPU. (Later, once interrupts are running, the real idle strategy is `hlt` *inside*
a loop so interrupts can still wake it — see chapter 6.)

### 4. Landing in C

`kernel/kernel.c` defines `kernel_main`. This is the top of the OS: from here on,
everything is (mostly) C. Per the build spec it will call, in order:
`gdt_init()`, `isr_install()`, `timer_init(100)`, `keyboard_init()`,
`screen_clear()`, print a welcome, `shell_init()`, then loop forever on `hlt`.
Each of those is a later chapter. The important thing right now is the *shape*:
`kernel_main` is the OS's `main()`, and its first half is pure **initialisation**
— building the tables and drivers that make the rest possible.

## The state of the world at `kernel_main`

Thanks to Multiboot, when your C code first runs you can assume:

- The CPU is in **32-bit protected mode** (not 16-bit real mode).
- A basic, temporary **GDT** is loaded (but you will replace it — chapter 2).
- **Interrupts are disabled** — nothing will interrupt you until you turn them on.
- Your kernel is loaded at physical address **1 MB** (set by `linker.ld`).
- `eax` holds a magic value confirming a Multiboot boot, and `ebx` points to an
  info structure (MiniOS ignores both, but a bigger kernel would read the memory
  map from `ebx`).

## Going further

- **Real bootloaders** (GRUB2, the Linux boot protocol, U-Boot on embedded) do
  far more: they read filesystems, show menus, load initramfs images, and pass
  hundreds of parameters. Multiboot spares MiniOS all of it.
- **UEFI** has largely replaced legacy BIOS on modern machines. It boots 64-bit
  code directly, uses a real filesystem (the EFI System Partition) instead of a
  512-byte boot sector, and calls a standardized `EFI_MAIN`. The *idea* is
  identical — a firmware that hands off to your code — but the contract is richer.
- **The 512-byte boot sector** is the classic "from scratch" path: the BIOS loads
  exactly one 512-byte sector ending in `0x55AA`, and *that* tiny program must
  bootstrap everything else, including the switch to protected mode. MiniOS skips
  this by using Multiboot; it is a great follow-up project.

### Exercises

1. In `boot.asm`, why must `MBOOT_CHECKSUM` be *negative*? Compute
   `magic + flags + checksum` by hand and confirm it is zero mod 2^32.
2. What would happen if you set `esp` to `stack_bottom` instead of `stack_top`?
3. The spec says `eax`/`ebx` carry Multiboot info. Where is the memory map, and
   what would you need it for? (Hint: chapter 5.)
