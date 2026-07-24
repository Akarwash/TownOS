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
            drivers/screen.c drivers/ports.c drivers/keyboard.c drivers/disk.c \
            libc/mem.c libc/string.c \
            shell/shell.c \
            user/user_program.c
ASM_SOURCES = boot/boot.asm kernel/gdt_flush.asm kernel/isr_stubs.asm

# Object files (replace .c with .o and .asm with .o)
C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)

ALL_OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# Default target
all: minios.bin

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

# Run in QEMU with the disk attached to the primary ATA bus.
#   if=ide      put the drive on the emulated IDE/ATA controller (NOT virtio or
#               AHCI), so it answers at I/O ports 0x1F0-0x1F7 where the driver looks
#   index=0     first drive on that controller = primary bus master
#   format=raw  the file is a flat byte array, no qcow layering
run: minios.bin $(DISK_IMG)
	$(QEMU) -kernel minios.bin -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk

# Clean build files
clean:
	rm -f $(ALL_OBJECTS) minios.bin minios.elf