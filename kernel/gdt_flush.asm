global gdt_flush

; ============================================================================
; void gdt_flush(uint64_t gdt_ptr_addr)
;   gdt_ptr_addr arrives in RDI (System V AMD64 calling convention).
;   Loads the new GDT, reloads the data segments, and reloads CS.
; ============================================================================
[bits 64]
gdt_flush:
    lgdt [rdi]              ; load the new GDT (16-bit limit + 64-bit base)

    ; Reload the data/stack segment registers with the kernel data selector.
    ; These take effect immediately; only CS is special.
    mov ax, 0x10           ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; --- Reload CS ---
    ; In 64-bit mode you CANNOT reload CS with `jmp 0x08:label` — a far jump with
    ; an immediate selector:offset is not encodable. The trick is a far RETURN,
    ; which loads CS and RIP together from the stack. `retfq` pops RIP first, then
    ; CS, so we must leave the stack as [RIP (top)][CS], and it will jump there.
    pop rax                ; RAX = caller's return address (currently on top of stack)
    push 0x08              ; push the new CS selector (kernel code)
    push rax               ; push the return address back, now above the selector
    retfq                  ; far return: CS <- 0x08, RIP <- RAX -> back to the caller
