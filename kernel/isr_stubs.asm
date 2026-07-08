; ============================================
; ISR and IRQ assembly stubs
;
; Each interrupt gets a tiny entry point that pushes a (dummy) error code
; and the interrupt number, then jumps to a shared stub that saves CPU
; state and calls into C.
; ============================================

extern isr_handler
extern irq_handler

; --- Macros -------------------------------------------------

; CPU exception that does NOT push an error code: push a dummy 0
; so every handler sees an identical stack layout.
%macro ISR_NOERR 1
    global isr%1
isr%1:
    cli
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro

; CPU exception where the CPU already pushed a real error code.
%macro ISR_ERR 1
    global isr%1
isr%1:
    cli
    push dword %1
    jmp isr_common_stub
%endmacro

; Hardware interrupt. %1 = IRQ number, %2 = remapped interrupt number (32-47).
%macro IRQ 2
    global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

; --- CPU exceptions (0-31) ----------------------------------
; Exceptions 8, 10, 11, 12, 13, 14, 17, 21 push an error code.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; --- Hardware IRQs (remapped to interrupts 32-47) -----------
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; --- Common stubs -------------------------------------------
; Stack when we arrive here (low -> high):
;   int_no, err_code, eip, cs, eflags, [user_esp, user_ss]

isr_common_stub:
    pusha                   ; edi, esi, ebp, esp, ebx, edx, ecx, eax
    mov ax, ds
    push eax                ; save the data segment selector

    mov ax, 0x10            ; load the kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; pass registers_t* to the C handler
    call isr_handler
    add esp, 4              ; discard the pushed argument

    pop eax                 ; restore the original data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8              ; drop int_no and err_code
    sti
    iret

irq_common_stub:
    pusha
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8
    sti
    iret
