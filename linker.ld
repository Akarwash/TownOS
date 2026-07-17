OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Program headers (load segments). Declaring them explicitly keeps the kernel
 * and the user program in SEPARATE PT_LOAD segments. Without this, the 3MB
 * virtual gap between the kernel (at 1M) and the user code (at 4M) would be
 * padded with zeros IN THE FILE, ballooning minios.bin by ~3MB. A distinct
 * segment lets each region carry its own file offset, so the gap costs nothing
 * on disk. */
PHDRS {
    kernel PT_LOAD FLAGS(7);      /* RWX: the kernel image at 1M */
    user   PT_LOAD FLAGS(7);      /* RWX: the ring-3 program at 4M */
}

SECTIONS {
    . = 1M;

    .multiboot : {
        *(.multiboot)
    } :kernel

    .text : {
        *(.text)
    } :kernel

    .rodata : {
        *(.rodata)
    } :kernel

    .data : {
        *(.data)
    } :kernel

    .bss : {
        *(COMMON)
        *(.bss)
    } :kernel

    /* --- User program --- */
    /* The ring-3 code lands at 4M, inside the PD[2] page (0x400000-0x5FFFFF)
     * that boot.asm marks PG_USER. Its own load segment (see PHDRS) so the gap
     * from the kernel is not written to the file. */
    . = 0x400000;
    .user_text : {
        _user_text_start = .;
        *(.user_text)
        _user_text_end = .;
    } :user
}
