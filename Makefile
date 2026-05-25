# Compilers and tools
CC = i686-elf-gcc
LD = i686-elf-ld
ASM = nasm
QEMU = qemu-system-i386

# Compiler flags
CFLAGS = -ffreestanding -m32 -fno-pie -nostdlib -nodefaultlibs -Wall -Wextra
ASMFLAGS = -f elf32
LDFLAGS = -T linker.ld -nostdlib

# Source files
C_SOURCES = kernel/kernel.c drivers/screen.c drivers/ports.c libc/mem.c
ASM_SOURCES = boot/boot.asm

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