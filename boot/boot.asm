; ============================================
; MiniOS Boot Entry Point (Multiboot compliant)
; ============================================

; --- Multiboot header constants ---
MBOOT_MAGIC     equ 0x1BADB002
MBOOT_PAGE_ALIGN equ 1 << 0
MBOOT_MEM_INFO  equ 1 << 1
MBOOT_FLAGS     equ MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO
MBOOT_CHECKSUM  equ -(MBOOT_MAGIC + MBOOT_FLAGS)

; --- Multiboot header ---
section .multiboot
    align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

; --- Stack ---
section .bss
    align 16
stack_bottom:
    resb 16384
stack_top:

; --- Entry point ---
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    call kernel_main
    cli
    hlt