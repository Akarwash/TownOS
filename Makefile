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
# NOTE: the build will NOT link successfully until the hand-written GDT / IDT /
# ISR stubs are filled in. boot/boot.asm's long-mode climb is now implemented, so
# the remaining undefined symbols are the ISR/IRQ entry points in
# kernel/isr_stubs.asm. That link failure is expected — see CONVERSION_NOTES.md.
# ============================================================================

# Compilers and tools
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm
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
            drivers/screen.c drivers/ports.c drivers/keyboard.c \
            libc/mem.c libc/string.c \
            shell/shell.c
ASM_SOURCES = boot/boot.asm kernel/gdt_flush.asm kernel/isr_stubs.asm

# Object files (replace .c with .o and .asm with .o)
C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.asm=.o)

ALL_OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# Default target
all: minios.bin

# Link everything into the final binary
minios.bin: $(ALL_OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble ASM files
%.o: %.asm
	$(ASM) $(ASMFLAGS) $< -o $@

# Run in QEMU
run: minios.bin
	$(QEMU) -kernel minios.bin

# Clean build files
clean:
	rm -f $(ALL_OBJECTS) minios.bin