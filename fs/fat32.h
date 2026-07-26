#ifndef FAT32_H
#define FAT32_H

#include "../include/types.h"

// ============================================================================
// A read-only FAT32 filesystem.
// ============================================================================
// The disk driver (drivers/disk.c) hands out numbered 512-byte blocks and knows
// nothing else: no names, no files, no notion of which blocks are in use. This
// layer is what gives those blocks meaning. It reads the bookkeeping the format
// leaves on the disk itself (a boot sector describing the layout, a table of
// cluster chains, directories full of name-to-cluster entries) and turns it into
// "read the file called HELLO.TXT".
//
// Read-only on purpose. Reading needs three things: parse the boot sector,
// follow cluster chains, parse directory entries. Writing needs all of that plus
// free-cluster search, chain updates, updating every FAT copy, directory entry
// updates, growing a directory, and ordering the writes so a crash midway cannot
// leave the FAT and the directory disagreeing. That is where filesystems get
// hard and where bugs corrupt data instead of merely failing. Reading is also
// the complete prerequisite for the next rung (loading a program off the disk),
// because a program image is read, never written. TODO(fat32-write).
//
// See docs/reference/fat32.md and docs/decisions/0014-read-only-fat32.md.

// Parse the boot sector and cache the volume geometry. Call once, after
// disk_init. Returns 0 on success, -1 if the disk is unreadable or does not hold
// a FAT32 volume this code can trust. Every other call here requires it.
int fat32_init(void);

// Print every entry in the root directory (name, and either a size in bytes or a
// <DIR> marker). Long-filename fragments, deleted entries, and the volume label
// are skipped, so what prints is what MiniOS can actually name. Returns 0 on
// success, -1 on a read error or a corrupt directory chain.
int fat32_list_root(void);

// Fill `buf` with the root directory's file names, one per line (each name
// followed by '\n'), keeping the buffer NUL-terminated, and report how many names
// were written through out_count (which may be NULL). This is the buffer-filling
// sibling of fat32_list_root, which prints instead: SYS_LIST uses it to hand a
// listing to a ring-3 program that cannot see the kernel's screen. If the names do
// not all fit, the trailing ones are dropped and the count reflects only those
// written. Returns 0 on success, -1 if the filesystem is not ready, buf is NULL or
// zero-sized, or the directory chain is corrupt.
int fat32_list_names(char *buf, uint32_t bufsize, uint32_t *out_count);

// Report the size in bytes of the file called `name`, without reading any of its
// contents (the size lives in the directory entry). This exists because reading a
// file means allocating a buffer for it first, which means knowing its size
// first. Returns 0 on success, -1 if the name is not 8.3, is not in the root
// directory, or names a directory.
int fat32_stat(const char *name, uint32_t *out_size);

// Read the file called `name` (an 8.3 name such as "HELLO.TXT", case
// insensitive) from the root directory into buf, and write its real length in
// bytes to out_size (which may be NULL). The buffer receives exactly that many
// bytes, trimmed from the last cluster, never the stale bytes that follow the
// end of the file. Returns 0 on success, -1 if the name is not 8.3, is not in
// the root directory, names a directory, does not fit in bufsize, or the volume
// is corrupt.
int fat32_read_file(const char *name, void *buf, uint32_t bufsize,
                    uint32_t *out_size);

#endif
