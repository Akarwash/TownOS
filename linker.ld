OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* One load segment: the kernel, and nothing else.
 *
 * There used to be a second :user segment here, holding the ring-3 programs at
 * 4M. User programs are now separately linked binaries that live on the disk
 * image (see user/user.ld) and are read in at runtime by the ELF loader, so
 * nothing ring-3 is part of this image any more. */
PHDRS {
    kernel PT_LOAD FLAGS(7);      /* RWX: the kernel image at 1M */
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

}
