#ifndef PAGING_H
#define PAGING_H

#include "../include/types.h"

// ============================================================================
// Per-task address spaces (per-process paging)
// ============================================================================
// Until now every task shared the single boot page-table tree (boot/boot.asm),
// all 2MB huge pages, identity mapped. That makes tasks really threads: two
// tasks cannot use the same virtual address for different memory, so their
// stacks had to sit at different addresses and any task could read any other's.
//
// This module gives each task its OWN page-table tree. Every tree has two halves
// (see docs/reference/paging.md):
//   - Kernel half: SHARED by reference, identical in every tree, kernel-only. The
//     kernel must be mapped in every tree because when an interrupt fires the CPU
//     jumps into kernel code WITHOUT changing CR3, so the first kernel instruction
//     is fetched through whatever (task's) tree is loaded. The clone copies the
//     boot PML4 entry, so every tree points at the SAME pdpt_table/pd_table.
//   - User half: PRIVATE per task, 4KB pages to freshly allocated frames, user
//     bit set. Two tasks can both use virtual 0x400000 and land on different
//     physical memory. That is the isolation.
//
// The user half MUST use 4KB pages: 2MB huge-page identity mapping means
// virtual==physical, which cannot give two tasks different physical memory at the
// same virtual address. So each per-task tree is MIXED page sizes: 4KB for the
// private user region, 2MB huge pages for the shared kernel region.

// A per-task address space. The whole point is the PML4 physical address, which
// is what gets loaded into CR3 on a task switch (CR3 holds a PHYSICAL address).
// Everything is identity mapped below 1GB, so pml4_phys doubles as the writable
// virtual address of the PML4 table itself.
typedef struct address_space {
    uint64_t pml4_phys;   // physical base of this tree's PML4 (goes into CR3)
} address_space_t;

// Build a fresh address space: allocate a zeroed PML4 and clone the kernel half
// by reference so the kernel is mapped in it (kernel-only), then return a handle.
// Returns NULL on out-of-memory. Implemented in paging.c.
address_space_t *paging_create_address_space(void);

// Map one 4KB page: walk (creating intermediate tables as needed) the PML4 ->
// PDPT -> PD -> PT path in `as` and set the leaf to `phys` with `flags`. `virt`
// and `phys` must be 4KB-aligned. `flags` are raw PTE flag bits (PG_* below).
// Returns 0 on success, non-zero on failure (out of frames). Implemented in
// paging.c. This is the workhorse for the private user half.
int paging_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags);

// Load `as` into CR3, switching the active address space. Writing CR3 flushes the
// TLB (we use no global pages), so stale translations are dropped automatically.
void paging_switch(address_space_t *as);

// Page-table entry flag bits, matching boot/boot.asm and kernel/memory.c. Shared
// so callers can spell out user-page flags (PG_PRESENT|PG_WRITABLE|PG_USER).
#define PG_PRESENT   (1UL << 0)   // entry is present
#define PG_WRITABLE  (1UL << 1)   // writes allowed
#define PG_USER      (1UL << 2)   // ring-3 (CPL 3) access allowed
#define PG_HUGE      (1UL << 7)   // PD entry: this IS a 2MB page, stop the walk

// The physical-frame field of a page-table entry: bits 12..51. Masking an entry
// with this drops the flag bits and leaves the frame address (which, because
// everything below 1GB is identity mapped, is also a writable virtual pointer to
// the next table). Exposed so a caller can walk a tree back by hand.
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000UL

#endif
