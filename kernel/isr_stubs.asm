; ============================================
; ISR and IRQ assembly stubs (64-bit)
; ============================================
;
; The CPU tells us WHICH interrupt fired only by WHERE it jumps: the IDT gate for
; vector N points at the address of stub N. There is no register that says "this
; is interrupt N". So we need one tiny entry point per vector, and each one
; hardcodes and pushes its own number before funnelling into a shared stub. That
; is the whole reason 48 near-identical labels exist below.
;
; CRITICAL COUPLING: the order in which the common stub pushes registers MUST
; mirror, field for field, the `registers_t` struct in kernel/isr.h. The compiler
; cannot see this relationship, so nothing warns you if it drifts. Get it wrong
; and every field the C handler reads is shifted, which normally ends in a silent
; triple fault. If you touch either side, touch both. (isr.h says so too.)

[bits 64]

extern isr_handler
extern irq_handler

; ============================================================================
; MANUAL COUPLING, NO COMPILER CHECK. KEEP IN SYNC WITH include/vectors.h.
;
; NASM cannot include a C header, so PIC_MASTER_VECTOR_BASE is duplicated here
; as an `equ`. The IRQ macro below derives every hardware vector from it, the
; same way vectors.h derives IRQ_TIMER, IRQ_KEYBOARD, and the rest. If you move
; the base in vectors.h, you MUST move it here too. Nothing will warn you if
; these two numbers drift apart; the symptom is IRQs landing on the wrong gate.
; This is the same class of invisible coupling as the registers_t push order.
; ============================================================================
PIC_MASTER_VECTOR_BASE equ 0x40

; --- Per-vector entry macros --------------------------------------------------
; For most vectors the CPU pushes no error code. For a handful of exceptions it
; pushes a real one. To give the C handler a single uniform stack layout, the
; no-error path pushes a dummy 0 in the error-code slot so both paths look the
; same from `registers_t`'s point of view.

%macro ISR_NOERR 1              ; exception WITHOUT a CPU error code
global isr%1
isr%1:
    push 0                      ; dummy error code, keeps the frame uniform
    push %1                     ; this stub's own vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1               ; exception where the CPU already pushed one
global isr%1
isr%1:
    push %1                     ; vector number only; real error code already on stack
    jmp isr_common_stub
%endmacro

%macro IRQ 1                   ; hardware IRQ %1, delivered at PIC base + %1
global irq%1
irq%1:
    push 0                      ; IRQs never carry an error code, push a dummy
    push PIC_MASTER_VECTOR_BASE + %1   ; remapped vector, derived from the base
    jmp irq_common_stub
%endmacro

; --- CPU exception entry points (vectors 0..31) -------------------------------
; Error-code-pushing vectors are 8, 10, 11, 12, 13, 14, 17. Everything else in
; this range gets a dummy via ISR_NOERR.
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
ISR_NOERR 21
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

; --- Hardware IRQ entry points (IRQ 0..15 -> vectors 0x40..0x4F) ---------------
; The PIC was remapped (see idt.c) so IRQ0 arrives as vector 0x40, IRQ1 as 0x41,
; and so on, clear of Intel's reserved 0..31 exception range. The vector is
; PIC_MASTER_VECTOR_BASE + the IRQ line; IRQ8..15 land at 0x48..0x4F, which the
; slave PIC base (0x48) in vectors.h names.
IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

; --- Register save / restore --------------------------------------------------
; `pusha` does not exist in 64-bit, so every general-purpose register is pushed
; by hand. The push order is the REVERSE of the field order in `registers_t`:
; after the final `push rdi`, RSP points exactly at the struct's first field
; (rdi), which makes RSP a valid `registers_t*`.
;
; registers_t field order (low address -> high address):
;   rdi rsi rbp rbx rdx rcx rax  r8 r9 r10 r11 r12 r13 r14 r15  int_no err_code
;   rip cs rflags user_rsp ss
; The last five are pushed by the CPU; int_no/err_code by the per-vector stubs;
; the fifteen below by these macros.

%macro PUSH_GPRS 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
%endmacro

%macro POP_GPRS 0
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

; --- Shared tails -------------------------------------------------------------
; Both stubs: save GPRs, hand the frame pointer to the C handler in RDI (the
; System V AMD64 first-argument register), restore GPRs, drop the vector number
; and error code (16 bytes), and return with iretq (the 64-bit interrupt return).

isr_common_stub:
    PUSH_GPRS
    mov rdi, rsp                ; registers_t* -> first argument
    call isr_handler
    POP_GPRS
    add rsp, 16                 ; discard pushed vector number + error code
    iretq

irq_common_stub:
    PUSH_GPRS
    mov rdi, rsp                ; registers_t* -> first argument
    call irq_handler
    POP_GPRS
    add rsp, 16                 ; discard pushed vector number + error code
    iretq
