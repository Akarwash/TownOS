// User program A: print "A" forever.
//
// Built as a standalone static ELF64 binary (see user/user.ld and the USER_*
// rules in the Makefile), copied onto the FAT32 disk image as A.ELF, and loaded
// at runtime by the kernel's ELF loader. It is NOT part of minios.bin: changing
// this file and rebuilding A.ELF changes what the machine runs, with no kernel
// rebuild.

#include "userlib.h"

// A zero-initialised global, so this program actually has a .bss segment: memory
// the ELF file describes but does not store. It exists to keep the loader's
// zero-fill honest, since a program with no .bss would load correctly even if
// that step were missing.
static volatile unsigned long iterations;

// The entry point named by user.ld's ENTRY(_start). The loader takes the entry
// address from the ELF header, so this symbol's name only has to match the
// linker script, not anything in the kernel.
void _start(void) {
    for (;;) {
        sys_write("A");
        iterations++;
        user_delay();
    }
}
