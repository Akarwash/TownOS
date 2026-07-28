// User program A: print "A" twenty times, then exit with status 0.
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

// How many letters to print before exiting. THE LOOP MUST BE BOUNDED. It used to
// run forever, which was fine when nothing waited for it; now the shell blocks in
// sys_wait until this program is done, and there is no way to kill a running task
// and no signals, so a program that never exits leaves the shell permanently
// unusable with no way back short of a reboot.
#define A_ROUNDS  20

// The entry point named by user.ld's ENTRY(_start). The loader takes the entry
// address from the ELF header, so this symbol's name only has to match the
// linker script, not anything in the kernel.
void _start(void) {
    for (unsigned long i = 0; i < A_ROUNDS; i++) {
        sys_write("A");
        iterations++;
        user_delay();
    }

    // A program ends ITSELF. There is no crt0 and nothing wraps _start, so falling
    // off the end here would return into whatever bytes happen to follow this
    // function rather than into anything that ends the task.
    sys_exit(0);
}
