// paging.c: per-task address spaces (per-process paging).
//
// See paging.h and docs/reference/paging.md for the two-halves model (private
// user half, shared kernel half) and why the kernel must be mapped in every tree.
// This is the densest code in the kernel and is read as learning material, so it
// is commented heavily and deliberately.

#include "paging.h"
#include "memory.h"
#include "heap.h"
#include "../libc/mem.h"
#include "../drivers/screen.h"

// The boot page directory (boot/boot.asm). Option A clones the kernel half BY
// VALUE: a per-task tree gets its own PML4/PDPT/PD, and its PD is filled with the
// SAME huge-page entries this boot pd_table holds (identical physical kernel
// frames, kernel-only), so every task maps the identical kernel at the identical
// addresses. We do NOT point at the one boot pd_table by reference, because in
// this kernel the user region at 0x400000 lives in pd_table[2]/[3]: sharing the
// page by reference would share the user huge pages too and make a private 4KB
// mapping at 0x400000 impossible. See docs/decisions/0012 for why by-reference
// did not fit and docs/reference/paging.md for the by-value invariant.
//
// TRIPWIRE (the load-bearing invariant): by-value cloning is correct ONLY because
// kernel mappings are FROZEN after boot. memory_detect_and_map fills the identity
// map once at startup, before any task tree exists, and nothing ever mutates a
// kernel PD entry afterward, so a by-value copy can never drift out of sync with
// the boot tables. If the kernel ever remaps itself at runtime (kernel ASLR,
// memory hot-plug, a higher-half relocation), the copies in existing task trees
// would silently go stale; at that point this MUST become a by-reference share of
// the kernel tables or gain an explicit propagation step. Do not add runtime
// kernel remapping without revisiting this.
extern uint64_t pd_table[512];

// One page directory is 512 entries (512 * 2MB = 1GB), matching kernel/memory.c.
#define PD_ENTRIES  512

// The two PD slots that describe the ring-3 region (boot.asm PD[2]/PD[3]):
//   PD[2]  0x400000-0x5FFFFF : user code
//   PD[3]  0x600000-0x7FFFFF : user stack
// The by-value kernel clone SKIPS these two so the user branch can be overridden
// with private 4KB page tables (paging_map_page) instead of the shared huge page.
#define USER_PD_INDEX_CODE   2
#define USER_PD_INDEX_STACK  3

// Extract the table index for each level from a virtual address. The x86-64 page
// walk chops the 48-bit virtual address into four 9-bit indices plus a 12-bit
// offset: PML4 (bits 47..39), PDPT (38..30), PD (29..21), PT (20..12).
#define PML4_INDEX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FF)

// Allocate a page-table frame and zero it. Page tables MUST be 4KB and 4KB-
// aligned, which is exactly what alloc_frame hands out (a whole frame), so tables
// come from the frame allocator, NOT kmalloc (kmalloc carves sub-page pieces off
// the heap slab and does not align to a frame). The returned frame is inside the
// identity-mapped pool (MEMORY_START..pool_top, capped at 1GB), so its physical
// address is also a writable virtual pointer, which is how we fill the table
// below. alloc_frame never returns a frame above the identity ceiling, so writing
// through the returned address can never fault; returns 0 only on out-of-memory.
static uint64_t *alloc_table(void) {
    uint64_t frame = alloc_frame();
    if (frame == 0) {
        return NULL;
    }
    memset((void *)frame, 0, FRAME_SIZE);
    return (uint64_t *)frame;
}

// Follow a table entry down one level, creating the next table if absent. When we
// create it, the entry is marked PRESENT | WRITABLE | USER: this is the AND-down
// rule (boot.asm relies on it too). x86 permits a ring-3 access only if the user
// bit is set at EVERY level of the walk, so the upper levels are made permissive
// and the actual privilege is gated at the leaf. Every table paging_map_page
// creates is on the user branch, so user-permissive intermediates are correct.
// Returns NULL if a new table could not be allocated.
static uint64_t *next_table(uint64_t *entry) {
    if (!(*entry & PG_PRESENT)) {
        uint64_t *table = alloc_table();
        if (table == NULL) {
            return NULL;
        }
        *entry = (uint64_t)table | PG_PRESENT | PG_WRITABLE | PG_USER;
    }
    return (uint64_t *)(*entry & PTE_ADDR_MASK);
}

address_space_t *paging_create_address_space(void) {
    // The handle is kernel-only bookkeeping (only paging/scheduler code reads it),
    // so it is safe on the kernel heap.
    address_space_t *as = (address_space_t *)kmalloc(sizeof(address_space_t));
    if (as == NULL) {
        return NULL;
    }

    // Own PML4, PDPT, and PD for the low 1GB (Option A). Page-table pages come
    // from alloc_frame (4KB, aligned), never kmalloc.
    uint64_t *pml4 = alloc_table();
    uint64_t *pdpt = alloc_table();
    uint64_t *pd   = alloc_table();
    if (pml4 == NULL || pdpt == NULL || pd == NULL) {
        // Best-effort cleanup: hand any frames we did get back to the pool. Tasks
        // are never destroyed once built, so this only runs on a create that fails
        // partway (out of memory), and keeping it simple is fine.
        if (pml4 != NULL) { free_frame((uint64_t)pml4); }
        if (pdpt != NULL) { free_frame((uint64_t)pdpt); }
        if (pd   != NULL) { free_frame((uint64_t)pd);   }
        kfree(as);
        return NULL;
    }

    // Clone the kernel half BY VALUE: copy every boot PD entry except the two user
    // slots. Present kernel entries carry the identical huge-page mapping (same
    // physical kernel frame, no user bit); absent entries copy as zero. This is
    // the copy the tripwire above governs. PD[2]/PD[3] are left zero (not present)
    // so paging_map_page can install private 4KB page tables there in Stage 3; a
    // ring-3 touch of an unmapped user address therefore faults, which is the
    // guard the old shared-region layout never had.
    for (int i = 0; i < PD_ENTRIES; i++) {
        if (i == USER_PD_INDEX_CODE || i == USER_PD_INDEX_STACK) {
            continue;
        }
        pd[i] = pd_table[i];
    }

    // Wire PML4[0] -> this PDPT -> this PD. Both upper entries are user-permissive
    // (AND-down rule); the kernel leaves in the PD stay kernel-only because their
    // copied values have no user bit, so ring 3 still cannot reach kernel memory
    // even though the branch above it says "user may pass".
    pdpt[0] = (uint64_t)pd   | PG_PRESENT | PG_WRITABLE | PG_USER;
    pml4[0] = (uint64_t)pdpt | PG_PRESENT | PG_WRITABLE | PG_USER;

    // CR3 wants a PHYSICAL address; identity mapping makes the pointer's value the
    // physical address already.
    as->pml4_phys = (uint64_t)pml4;
    return as;
}

int paging_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)as->pml4_phys;

    // Walk the 4KB path, creating PDPT/PD/PT as needed. For the user branch the
    // upper two levels already exist from create (present, user); PD[2]/PD[3] and
    // the PT below are created here on first touch.
    uint64_t *pdpt = next_table(&pml4[PML4_INDEX(virt)]);
    if (pdpt == NULL) {
        return 1;
    }
    uint64_t *pd = next_table(&pdpt[PDPT_INDEX(virt)]);
    if (pd == NULL) {
        return 1;
    }
    uint64_t *pt = next_table(&pd[PD_INDEX(virt)]);
    if (pt == NULL) {
        return 1;
    }

    // Set the leaf. flags carries the real privilege of the page (the caller
    // passes PG_PRESENT | PG_WRITABLE | PG_USER for a ring-3 page).
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    return 0;
}

// Load an address space into CR3. CR3 holds a PHYSICAL address; everything below
// 1GB is identity mapped, so as->pml4_phys is already both the physical base of
// the PML4 and a writable virtual pointer to it. Writing CR3 flushes the TLB
// (we mark no pages PG_GLOBAL, so no entries survive the write), which is exactly
// what drops the previous task's stale translations on a switch.
void paging_switch(address_space_t *as) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}
