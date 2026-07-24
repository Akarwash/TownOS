/* Linker script for a MiniOS user program.
 *
 * A user program is now a SEPARATE binary from the kernel: it is compiled and
 * linked on its own, ends up as an ELF64 file on the FAT32 disk image, and is
 * read and loaded at runtime by kernel/elf.c. Nothing here is linked into
 * minios.bin. See docs/reference/elf-loading.md.
 *
 * Load address 0x400000 (4MB), the start of the ring-3 region
 * (USER_REGION_START in kernel/memory.h). There is no relocation in the loader,
 * so this address is fixed: the program must be linked at exactly the address it
 * will be loaded at.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Two load segments so the loader has something real to distinguish. Only the
 * write bit is actually enforceable in the page tables (MiniOS does not enable
 * NX), so R+X and R mean the same thing to the hardware here; what matters is
 * that the text segment is mapped WITHOUT the writable bit and the data segment
 * with it. */
PHDRS {
    text PT_LOAD FLAGS(5);      /* R+X: code and its read-only strings */
    data PT_LOAD FLAGS(6);      /* R+W: writable data and .bss */
}

SECTIONS {
    . = 0x400000;

    /* CONTRACT WITH THE LOADER: every loadable segment starts on a 4096-byte
     * boundary AND is a whole number of 4096-byte pages long. kernel/elf.c
     * REQUIRES this and rejects a file that violates it.
     *
     * It is enforced here rather than handled there on purpose. The loader maps
     * whole pages, so a segment starting mid-page or ending mid-page would mean
     * two segments sharing a page, and the second map of that page would have to
     * merge rather than replace, with the stricter permissions winning. That is
     * real work in the kernel to support something we control completely from
     * here. The trailing `. = ALIGN(4096)` inside each output section is what
     * rounds the segment's size up, not just its start. */

    .text ALIGN(4096) : {
        *(.text*)
        . = ALIGN(4096);
    } :text

    .rodata ALIGN(4096) : {
        *(.rodata*)
        . = ALIGN(4096);
    } :text

    .data ALIGN(4096) : {
        *(.data*)
        . = ALIGN(4096);
    } :data

    /* .bss is NOBITS: it occupies memory but is not stored in the file, which is
     * what makes a segment's memory size larger than its file size. The loader
     * must zero that gap; see the zero-fill in kernel/elf.c. */
    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss*)
        . = ALIGN(4096);
    } :data

    /* Debug and comment sections would otherwise be assigned to the last segment
     * and inflate it. Drop everything not needed to run. */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
