# ============================================================================
# Cross toolchain install (x86_64-elf) — this repo builds a freestanding
# x86-64 kernel and needs a cross compiler that targets bare metal, not the host.
#
# This machine (macOS / Apple Silicon, Homebrew) — the following was run and works:
#   brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu
#   # verified: x86_64-elf-gcc 16.1.0, x86_64-elf-ld (binutils) 2.46.1,
#   #           nasm 3.01, qemu 11.0.0
#
# Debian / Ubuntu (reference — not run here):
#   sudo apt install nasm qemu-system-x86
#   # No distro package ships an x86_64-elf cross gcc/binutils; build from source
#   # (see https://wiki.osdev.org/GCC_Cross-Compiler) targeting --target=x86_64-elf,
#   # or on 64-bit hosts the native gcc/ld can be used with the flags below.
#
# The kernel links and boots. `make` produces two artifacts:
#   minios.elf  the linked ELF64 image, with 64-bit symbols for gdb
#   minios.bin  the same image repackaged as a 32-bit ELF, which is what QEMU's
#               Multiboot -kernel loader accepts (see the minios.bin rule below)
# ============================================================================

# Compilers and tools
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm
OBJCOPY = x86_64-elf-objcopy
QEMU = qemu-system-x86_64

# Compiler flags
#   -m64            build 64-bit code
#   -mno-red-zone   the red zone is unsafe once interrupts run in kernel mode
#   -mcmodel=kernel code/data live in the negative 2GB; required for a 64-bit kernel
CFLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=kernel -fno-pie -nostdlib -nodefaultlibs -Wall -Wextra
ASMFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib

# Source files
C_SOURCES = kernel/kernel.c kernel/gdt.c kernel/idt.c kernel/isr.c kernel/timer.c kernel/memory.c \
            kernel/usermode.c kernel/syscall.c kernel/scheduler.c kernel/heap.c kernel/paging.c \
            kernel/elf.c \
            drivers/screen.c drivers/ports.c drivers/keyboard.c drivers/disk.c \
            fs/fat32.c \
            libc/mem.c libc/string.c
ASM_SOURCES = boot/boot.asm kernel/gdt_flush.asm kernel/isr_stubs.asm

# Object files (replace .c with .o and .asm with .o)
C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)

ALL_OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# ---------------------------------------------------------------------------
# User programs: separate binaries, not part of the kernel image
# ---------------------------------------------------------------------------
# Each user program is compiled and linked on its own into a static ELF64 file
# that lands on the disk image. The kernel reads and loads it at runtime, so
# changing a program means rebuilding one small binary and copying it onto the
# image, not rebuilding the kernel.
#
#   -mcmodel=small  NOT -mcmodel=kernel. The kernel model assumes every symbol
#                   lives in the top 2GB of the address space; user code links at
#                   0x400000, nowhere near that, and the kernel model produces
#                   relocation errors on it.
#   -static         no dynamic linking; the loader resolves nothing at runtime.
#   -nostdlib       no host libc and no startup files. The entire runtime a
#                   program gets is user/userlib.h.
#   -fno-pie -no-pie  position DEPENDENT. The loader does not relocate, so the
#                   program must be linked at the exact address it loads at.
USER_CFLAGS = -ffreestanding -m64 -mno-red-zone -mcmodel=small -fno-pie -no-pie \
              -nostdlib -nodefaultlibs -static -Wall -Wextra
USER_LD_SCRIPT = user/user.ld

# -z max-page-size=4096 keeps the linker from padding segments out to its default
# 2MB alignment, which would bloat each binary enormously for no benefit here.
USER_LDFLAGS = -T $(USER_LD_SCRIPT) -Wl,-z,max-page-size=4096 -Wl,--build-id=none

# 8.3 uppercase names because the filesystem reads 8.3 names only, and the source
# file names match the on-disk names so the mapping needs no explaining. SHELL.ELF
# is the exception: its source is user/shell.c (lowercase), so it needs the explicit
# rule below rather than the pattern rule, which would look for user/SHELL.c and
# only resolve to shell.c on a case-insensitive filesystem.
USER_PROGRAMS = user/A.ELF user/B.ELF user/C.ELF user/SHELL.ELF

user/%.ELF: user/%.c user/userlib.h $(USER_LD_SCRIPT) include/syscalls.h include/vectors.h
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $<

# The interactive shell. Same recipe as the pattern rule, but the target and source
# names differ in case, so it is spelled out explicitly and portably.
user/SHELL.ELF: user/shell.c user/userlib.h $(USER_LD_SCRIPT) include/syscalls.h include/vectors.h
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $<

# Default target
all: minios.bin $(USER_PROGRAMS)

# Link everything into the ELF64 image. This keeps the 64-bit symbols gdb needs.
minios.elf: $(ALL_OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

# Repackage the ELF64 as a 32-bit ELF for booting.
#   QEMU's built-in Multiboot -kernel loader rejects an ELF64 image ("Cannot load
#   x86-64 image, give a 32bit one"). Our entry code (boot/boot.asm) starts in
#   32-bit protected mode and climbs to long mode itself, and every address in the
#   image lives in low memory, so relabelling the container as elf32-i386 is
#   accepted by the loader and boots correctly. The code is unchanged; only the
#   ELF header class differs. gdb should point at minios.elf for symbols.
minios.bin: minios.elf
	$(OBJCOPY) -O elf32-i386 $< $@

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble ASM files
%.o: %.asm
	$(ASM) $(ASMFLAGS) $< -o $@

# Disk image for the ATA driver and the FAT32 filesystem, created once if absent.
#
# 64MB, not 16MB: FAT32 is only legal with at least 65525 clusters, and 16MB
# cannot reach that with a sane cluster size (it would need 256-byte clusters,
# which is smaller than a block). Formatting tools refuse a 16MB FAT32 volume or
# silently hand back FAT16 instead. 64MB clears the bar comfortably at one block
# per cluster.
#
# tools/mkdisk.sh formats the image and copies in the test files with mtools (no
# sudo, no mounting). The rule has no prerequisites, so make skips it whenever
# disk.img already exists, and the script bails out too: reformatting on every
# `make run` would silently destroy the disk's contents.
DISK_IMG = disk.img
DISK_SIZE = 64M

$(DISK_IMG):
	./tools/mkdisk.sh $(DISK_IMG) $(DISK_SIZE)

# Copy the user programs onto the image. Deliberately PHONY, so it runs on every
# `make run` even when the image already exists.
#
# The image itself must be created once and then left alone (reformatting would
# destroy its contents), but the program binaries are build output and must never
# be stale: running an old A.ELF because the image was not refreshed looks exactly
# like a loader bug and costs an afternoon. mcopy -o overwrites without asking.
.PHONY: disk-programs
disk-programs: $(DISK_IMG) $(USER_PROGRAMS)
	mcopy -o -i $(DISK_IMG) $(USER_PROGRAMS) ::/

# Run in QEMU with the disk attached to the primary ATA bus.
#   if=ide      put the drive on the emulated IDE/ATA controller (NOT virtio or
#               AHCI), so it answers at I/O ports 0x1F0-0x1F7 where the driver looks
#   index=0     first drive on that controller = primary bus master
#   format=raw  the file is a flat byte array, no qcow layering
run: minios.bin disk-programs
	$(QEMU) -kernel minios.bin -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk

# Clean build files. The disk image is NOT removed: it is not build output, it is
# the machine's disk, and the test files on it were put there by hand.
clean:
	rm -f $(ALL_OBJECTS) minios.bin minios.elf $(USER_PROGRAMS)