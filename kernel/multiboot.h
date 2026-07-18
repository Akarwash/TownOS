#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "../include/types.h"

// Multiboot 1 structures, defined only to the depth this kernel needs (the
// memory map). The bootloader fills the info structure and leaves a physical
// pointer to it in EBX; boot/boot.asm forwards that pointer to kernel_main.

// flags bits in multiboot_info_t.flags that gate which fields are valid.
#define MULTIBOOT_INFO_MEMORY   (1 << 0)   // mem_lower / mem_upper are valid
#define MULTIBOOT_INFO_MEM_MAP  (1 << 6)   // mmap_length / mmap_addr are valid

// mmap entry types. Only type 1 is usable RAM; everything else is reserved,
// ACPI, or otherwise off-limits and must not be handed out as free frames.
#define MULTIBOOT_MEMORY_AVAILABLE  1

// Only the leading fields up to the memory map are declared; later fields
// (drives, config table, boot loader name, ...) exist in the real structure but
// are not needed here.
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;    // total bytes of all mmap entries (valid if flags bit 6)
    uint32_t mmap_addr;      // physical address of the first mmap entry
} multiboot_info_t;

// One entry in the memory map. Packed because base_addr is a u64 sitting at
// offset 4 (right after the u32 size): without packing the compiler would insert
// 4 bytes of padding to 8-align base_addr and the layout would no longer match
// what the bootloader wrote.
typedef struct {
    uint32_t size;           // bytes in THIS entry, NOT counting the size field itself
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;           // MULTIBOOT_MEMORY_AVAILABLE (1) = usable RAM
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif
