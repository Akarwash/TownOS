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

#endif
