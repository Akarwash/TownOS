#ifndef ELF_H
#define ELF_H

#include "../include/types.h"

// ============================================================================
// An ELF64 program loader.
// ============================================================================
// Until now a "program" was a range of bytes the linker welded into the kernel
// image, and running one meant copying it out of the kernel. This module reads a
// program from a FILE instead: a separately compiled, statically linked ELF64
// binary that lives on the FAT32 disk image.
//
// An ELF file is a manifest with payload attached. The part a loader cares about
// is small: the header says "this is a 64-bit x86-64 executable and its first
// instruction is at address E", and the program headers say "copy N bytes from
// file offset O to address V, and make the region M bytes long". Everything else
// in the format (section headers, symbol tables, relocation entries) is for
// linkers and debuggers and is ignored here.
//
// That manifest comes out of an untrusted file, and it is literally a list of
// instructions of the form "write these bytes to this address". Every field is
// validated before it is used and every destination is bounds-checked before
// anything is written. See docs/reference/elf-loading.md and
// docs/decisions/0015-elf-program-loading.md.

// Validate an ELF64 executable and report its entry point, without loading it.
// Checks the identification, class, endianness, machine and type, then every
// program header against the file's own bounds and the page-alignment contract
// with user/user.ld. Prints a specific reason and returns -1 on the first
// failure, so a malformed file is rejected rather than interpreted. Returns 0 on
// success. `name` is used only in messages.
int elf_check(const void *file, uint32_t file_size, const char *name,
              uint64_t *out_entry);

// Print the manifest of a validated ELF file: its entry point, and for each
// loadable segment the file offset, virtual address, file size and memory size.
// This is what a loader acts on, so being able to see it is the difference
// between debugging a loader and guessing at one.
void elf_print_manifest(const void *file, uint32_t file_size, const char *name);

#endif
