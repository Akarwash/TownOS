// paging.c: per-task address spaces (per-process paging).
//
// See paging.h and docs/reference/paging.md for the two-halves model (private
// user half, shared kernel half) and why the kernel must be mapped in every tree.
// This is the densest code in the kernel and is read as learning material, so it
// is commented heavily and deliberately.

#include "paging.h"
#include "memory.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"

// The boot page tables (boot/boot.asm), now all three global. We clone the
// kernel half of the address space out of pml4_table: a new per-task tree copies
// pml4_table[0], so its first PML4 entry points at the SAME pdpt_table/pd_table
// the boot tree uses. That is how the kernel (and the whole identity map) stays
// mapped, identically, in every task's tree without deep-copying anything.
extern uint64_t pml4_table[512];
extern uint64_t pdpt_table[512];
extern uint64_t pd_table[512];

// Load an address space into CR3. CR3 holds a PHYSICAL address; everything below
// 1GB is identity mapped, so as->pml4_phys is already both the physical base of
// the PML4 and a writable virtual pointer to it. Writing CR3 flushes the TLB
// (we mark no pages PG_GLOBAL, so no entries survive the write), which is exactly
// what drops the previous task's stale translations on a switch.
void paging_switch(address_space_t *as) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}
