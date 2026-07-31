#ifndef FAT32_H
#define FAT32_H

#include "../include/types.h"

// ============================================================================
// A read/write FAT32 filesystem.
// ============================================================================
// The disk driver (drivers/disk.c) hands out numbered 512-byte blocks and knows
// nothing else: no names, no files, no notion of which blocks are in use. This
// layer is what gives those blocks meaning. It reads the bookkeeping the format
// leaves on the disk itself (a boot sector describing the layout, a table of
// cluster chains, directories full of name-to-cluster entries) and turns it into
// "read the file called HELLO.TXT" — and now "write it" and "delete it" too.
//
// Writing is where a filesystem gets genuinely hard, because a bug corrupts data
// instead of merely failing, and the damage outlives the fix: a FAT scrambled by
// a bad write stays scrambled after the code is corrected. Two things keep it
// safe here. The FAT writer updates every copy of the table, not just the first
// (fat32_set_entry), so the redundant copies never disagree. And every mutation
// follows a strict write-before-publish order: new clusters and their data are
// laid down first, and a single directory-entry write is what makes them the
// file's contents, so a crash at any earlier point loses only unreferenced
// clusters and never damages the file already on disk.
//
// The scope is deliberately small: whole-file writes with no handles, no seek and
// no append; 8.3 names; the root directory only; the first FAT copy consulted for
// reads but every copy written; no timestamps. The reasoning is in
// docs/decisions/0020-writable-fat32.md; the original read-only scope, now
// superseded, is docs/decisions/0014-read-only-fat32.md.
//
// See docs/reference/fat32.md for the on-disk layout and the write path.

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

// Write `buf` (len bytes) to the file called `name` (an 8.3 name) in the root
// directory, creating it or replacing it whole. There are no handles, no seek and
// no append: the file's contents become exactly these len bytes. A replacement is
// crash-safe — the new clusters and data are written first and a single directory
// entry write commits them, so a failure midway loses only the new clusters and
// never damages the file already on disk. len == 0 creates an empty file that
// owns no clusters. Returns 0 on success, -1 if the filesystem is not ready, the
// name is not 8.3, `name` is an existing directory, the volume is out of space,
// or a disk operation fails.
int fat32_write_file(const char *name, const void *buf, uint32_t len);

// Count every free cluster on the volume by walking the whole FAT. This is a full
// recount that trusts no cached total, so it is the independent yardstick a leak
// test measures against: allocate and free the same file repeatedly and this must
// return to its starting value. Slow by design and off the allocation path.
// Returns the free-cluster count, or 0 if the filesystem is not ready.
uint32_t fat32_free_count(void);

// Delete the file called `name` from the root directory: free its cluster chain,
// then mark its directory entry deleted. Freeing the data before unpublishing the
// name is deliberate — a crash in between wastes space (a lost chain) rather than
// leaving a live entry pointing at freed clusters (corruption). Returns 0 on
// success, -1 if the filesystem is not ready, the name is not 8.3, is not in the
// root directory, names a directory, or a disk operation fails.
int fat32_delete(const char *name);

#endif
