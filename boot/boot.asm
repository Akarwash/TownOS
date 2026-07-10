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
; GRUB/QEMU hand control to us in 32-bit protected mode, so the header itself
; is unchanged by the 64-bit port. The climb to long mode happens below.
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

; ============================================================
; TODO(long-mode): 32 -> 64 climb goes here (HAND-WRITTEN).
;   The bootloader drops us in 32-bit protected mode. Before we can call the
;   64-bit C kernel we must, in 32-bit code:
;     - build the initial page tables (PML4/PDPT/PD) identity-mapping low memory
;     - set CR4.PAE, load CR3 with the PML4
;     - set EFER.LME (via the EFER MSR) to request long mode
;     - set CR0.PG to enable paging, which activates long mode
;     - load a 64-bit GDT and far-jump into a 64-bit code segment
;   None of this is generated here on purpose — it is being written by hand as
;   a learning exercise. Left as a stub.
; ============================================================

; --- Entry point ---
section .text
global _start
extern kernel_main

; TODO(long-mode): placeholder entry point — INCOMPLETE.
;   In the final version _start runs in 32-bit mode, performs the climb above,
;   and only the 64-bit continuation sets RSP = stack_top and calls kernel_main.
;   The body below is intentionally left unimplemented.
_start:
    ; TODO(long-mode): perform the 32->64 climb, then in 64-bit code:
    ;     mov rsp, stack_top
    ;     call kernel_main
    cli
    hlt
