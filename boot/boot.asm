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

; --- Paging / control-register flag bits ---
; Named here (instead of bare magic numbers inline) so the climb below reads as
; prose. Each bit is explained where it is used.
PG_PRESENT      equ 1 << 0      ; page-structure entry: mapping is present
PG_WRITABLE     equ 1 << 1      ; page-structure entry: writes allowed
PG_USER         equ 1 << 2      ; page-structure entry: ring-3 (CPL 3) access allowed
PG_HUGE         equ 1 << 7      ; PD entry: this IS the page (2MB), stop the walk
CR4_PAE         equ 1 << 5      ; CR4.PAE: Physical Address Extension (required for long mode)
EFER_MSR        equ 0xC0000080  ; Extended Feature Enable Register MSR number
EFER_LME        equ 1 << 8      ; EFER.LME: Long Mode Enable
CR0_PG          equ 1 << 31     ; CR0.PG: enable paging (this is what activates long mode)

; --- Stack ---
section .bss
    align 16
stack_bottom:
    resb 16384              ; 16 KB kernel stack
stack_top:

; --- Initial page tables ---
; Three levels only (PML4 -> PDPT -> PD): we use 2MB pages, so the walk stops at
; the PD and there is no PT level. Each table is exactly one 4KB page and MUST be
; 4096-aligned (the CPU ignores the low 12 bits of a table's physical address).
    align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
; pd_table is exposed to C (global) so kernel/memory.c can extend the identity
; map past the fixed boot window once it has read the real amount of RAM from the
; Multiboot map. The boot climb fills only the low entries; C fills the rest.
global pd_table
pd_table:
    resb 4096

; ============================================================
; Entry point — the 32 -> 64 long-mode climb (HAND-WRITTEN).
;   The bootloader drops us here in 32-bit protected mode with paging OFF. Long
;   mode requires paging ON, so we must build valid page tables BEFORE we flip
;   the switch: the instruction right after CR0.PG is set is fetched through the
;   very mapping we install below. Get the identity map wrong and the CPU triple
;   faults on its own next instruction.
; ============================================================
section .text
[bits 32]
global _start
extern kernel_main

_start:
    ; --- Step 1: zero all three tables (12 KB) ---
    ; The bootloader is NOT trusted to have zeroed .bss. A single stray byte that
    ; happens to set a present bit is a bogus mapping and a silent triple fault,
    ; so we clear all 12 KB by hand. The three tables are contiguous in .bss.
    mov edi, pml4_table
    xor eax, eax
    mov ecx, (4096 * 3) / 4         ; 3072 dwords = 12 KB
    rep stosd

    ; --- Step 2: PML4[0] -> PDPT ---
    ; The top-level table's first entry points at the next level down and marks
    ; it present + writable + USER. We only ever touch entry 0: the identity map
    ; lives entirely in the low 512GB the first PML4 entry covers.
    ;
    ; Why user here? The x86 page walk ANDs the user bit at EVERY level: ring-3
    ; access is only permitted if PML4, PDPT, AND the PD leaf all set it. So we
    ; make the two upper levels PERMISSIVE (user allowed) and gate real access at
    ; the leaves below. PD[0]/PD[1] omit user -> the kernel's own pages stay
    ; ring-0-only even though the branch above them says "user may pass."
    mov eax, pdpt_table
    or eax, PG_PRESENT | PG_WRITABLE | PG_USER
    mov [pml4_table], eax

    ; --- Step 3: PDPT[0] -> PD ---
    ; Same idea one level down: the first PDPT entry points at the page directory,
    ; also permissive (user) so the branch stays open to the user leaves below.
    mov eax, pd_table
    or eax, PG_PRESENT | PG_WRITABLE | PG_USER
    mov [pdpt_table], eax

    ; --- Step 4: PD[0..15] = identity-map the first 32 MB with 2MB pages ---
    ; Entry N maps virtual (N * 2MB) straight to physical (N * 2MB): fake address
    ; == real address. The PG_HUGE bit is MANDATORY: it tells the CPU "this entry
    ; is a 2MB page, do not walk to a PT." Omit it and the CPU chases a fourth
    ; level that does not exist.
    ;
    ; This is a FIXED, safe map built with no dependency on data we have not read
    ; yet (the Multiboot memory map is parsed later, in C). 32MB is deliberately
    ; larger than the kernel needs so C has headroom to work in before it extends
    ; the map to cover real RAM. C only ever adds entries at 32MB and up; it never
    ; touches these low entries that map the running kernel.
    ;
    ; Privilege is decided at the leaf (permission ANDs down the walk):
    ;   PD[0]  0x000000-0x1FFFFF : kernel (no user) - kernel code/data, VGA, stack
    ;   PD[1]  0x200000-0x3FFFFF : kernel (no user)
    ;   PD[2]  0x400000-0x5FFFFF : USER   - the ring-3 program's code lives here
    ;   PD[3]  0x600000-0x7FFFFF : USER   - the ring-3 program's stack lives here
    ;   PD[4..15] 0x800000-0x1FFFFFF : kernel (no user) - spare kernel RAM
    ; PD[0..3] are written explicitly so the privilege of the low 8MB (and the
    ; user region in particular) is visible per line; only 4-8M is user.
    mov dword [pd_table + 0 * 8], 0x000000 | PG_PRESENT | PG_WRITABLE | PG_HUGE
    mov dword [pd_table + 1 * 8], 0x200000 | PG_PRESENT | PG_WRITABLE | PG_HUGE
    mov dword [pd_table + 2 * 8], 0x400000 | PG_PRESENT | PG_WRITABLE | PG_HUGE | PG_USER
    mov dword [pd_table + 3 * 8], 0x600000 | PG_PRESENT | PG_WRITABLE | PG_HUGE | PG_USER

    ; PD[4..15] map 8MB..32MB, kernel-only. A loop is fine here: the privilege
    ; pattern (kernel, no user bit) is uniform across all of them.
    mov ecx, 4                  ; first entry to fill (8MB)
    mov eax, 0x800000           ; physical base of PD[4]
.map_fixed:
    mov edx, eax
    or  edx, PG_PRESENT | PG_WRITABLE | PG_HUGE
    mov [pd_table + ecx * 8], edx   ; low dword only; high dword stays zero (rep stosd)
    add eax, 0x200000           ; next 2MB page
    inc ecx
    cmp ecx, 16                 ; stop after PD[15] (32MB)
    jb  .map_fixed

    ; --- Step 5: enable PAE (CR4.PAE) ---
    ; Long mode requires Physical Address Extension; without it CR0.PG below just
    ; turns on 32-bit paging.
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    ; --- Step 6: point CR3 at the PML4 ---
    ; CR3 holds the physical base of the top-level table. Identity-mapped, so the
    ; label's address is already its physical address.
    mov eax, pml4_table
    mov cr3, eax

    ; --- Step 7: request long mode (EFER.LME) ---
    ; EFER is an MSR: read it (rdmsr -> edx:eax) selected by ECX, set the Long
    ; Mode Enable bit, write it back. This only ARMS long mode; paging activates it.
    mov ecx, EFER_MSR
    rdmsr
    or eax, EFER_LME
    wrmsr

    ; --- Step 8: enable paging (CR0.PG) ---
    ; The moment this store retires, PAE + LME + a loaded CR3 make the CPU enter
    ; long mode and address translation starts applying to our own instruction
    ; fetches. Step 4's identity map is why the next fetch still lands on real code.
    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax

    ; --- Step 9: load the bootstrap GDT and far-jump into 64-bit code ---
    ; We are now in long mode but still in a 32-bit (compatibility) code segment.
    ; A far jump that reloads CS with a 64-bit code selector is the only way to
    ; switch the CPU to true 64-bit execution.
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

[bits 64]
long_mode_start:
    ; --- Step 10: settle segment registers and hand off to C ---
    ; In long mode the data segments are effectively ignored, but stale 32-bit
    ; selectors are untidy, so load the flat data selector into all of them.
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top          ; establish the 64-bit stack

    ; Hand the Multiboot info pointer to C. The bootloader left the magic number
    ; in EAX and a physical pointer to the info structure in EBX when it entered
    ; _start. Nothing in the climb above touches EBX, so it is still valid here
    ; (a low physical address, and everything is identity-mapped, so it stays
    ; valid as a pointer). System V AMD64 passes the first integer argument in
    ; RDI; a 32-bit mov zero-extends EBX into RDI, giving a clean 64-bit pointer.
    mov edi, ebx
    call kernel_main            ; kernel_main(uint64_t multiboot_info_addr)

    ; --- Step 11: kernel_main should never return; if it does, halt forever ---
.hang:
    cli
    hlt
    jmp .hang

; --- Bootstrap GDT ---
; This is a BOOTSTRAP GDT, local to boot.asm. Its only job is to make the far
; jump above legal: a null descriptor plus one 64-bit code and one flat data
; descriptor. It is SEPARATE from kernel/gdt.c, which installs the real kernel
; GDT later. Do not try to share or unify the two — this one exists purely to
; get us into 64-bit mode, after which the C code replaces it.
section .rodata
gdt64:
    dq 0                                            ; null descriptor
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) ; exec | descriptor | present | 64-bit (L)
.data: equ $ - gdt64
    dq (1 << 41) | (1 << 44) | (1 << 47)             ; writable | descriptor | present
.pointer:
    dw $ - gdt64 - 1                                ; limit (size - 1)
    dq gdt64                                        ; base
