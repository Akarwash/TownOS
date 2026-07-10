global gdt_flush

; TODO(long-mode): 64-bit gdt_flush (HAND-WRITTEN).
;   Takes the address of a gdt_ptr_t in RDI (System V AMD64 calling convention)
;   and must:
;     - lgdt [rdi]
;     - reload the data segment registers with the kernel data selector
;     - far-jump / far-return to reload CS with the 64-bit kernel code selector
;   The exact selector reload sequence differs from 32-bit (no direct
;   `jmp 0x08:label`; a push+retfq or lretq is used) and is being written by
;   hand. Left as a stub.
gdt_flush:
    ret
