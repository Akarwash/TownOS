#ifndef ELF_H
#define ELF_H

#include "../include/types.h"
#include "paging.h"

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

// Load an ELF64 executable held in `file` into the address space `as`, and
// report its entry point.
//
// Validates the whole file first (identification, class, endianness, machine,
// type, and every program header against the file's own bounds and the
// page-alignment contract with user/user.ld), then for each PT_LOAD segment
// bounds-checks the destination, allocates and maps frames, copies the file
// bytes, and zeroes the gap up to the segment's memory size.
//
// Returns 0 on success, -1 on the first problem, having printed a specific
// reason naming the file. A rejected file must never take the kernel down with
// it. `name` is used only in messages.
int elf_load(const void *file, uint32_t file_size, address_space_t *as,
             const char *name, uint64_t *out_entry);

// The same, for a program stored on the FAT32 disk: stat it, read the whole file
// into a temporary heap buffer, load it, and free the buffer. Returns 0 on
// success, -1 (with a printed reason) if the file is missing, unreadable, or not
// a program this loader accepts.
int elf_load_file(const char *name, address_space_t *as, uint64_t *out_entry);

#endif
