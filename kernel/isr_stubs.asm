; ============================================
; ISR and IRQ assembly stubs (64-bit) — STUB, HAND-WRITTEN
; ============================================
;
; TODO(long-mode): 64-bit ISR/IRQ entry points and common stubs.
;
; This whole file is intentionally empty of implementation. In 64-bit the
; save/restore sequence is different from the 32-bit version and must be
; written by hand:
;   - `pusha` / `popa` DO NOT EXIST in 64-bit; every GPR (rax..r15) is pushed
;     and popped individually, in the exact order matching `registers_t`.
;   - the CPU pushes a larger interrupt frame (rip, cs, rflags, rsp, ss) and,
;     for some vectors, an error code; the dummy-error-code trick still applies.
;   - handlers receive registers_t* in RDI per the System V AMD64 ABI, not via
;     a pushed stack pointer argument.
;   - return is `iretq`, not `iret`.
;
; What must eventually be defined here (referenced extern from isr.c):
;   isr0..isr31          CPU exception entry points
;   irq0..irq15          hardware interrupt entry points (vectors 32..47)
;   isr_common_stub      shared save -> call isr_handler -> restore -> iretq
;   irq_common_stub      shared save -> call irq_handler  -> restore -> iretq
;
; extern isr_handler
; extern irq_handler
;
; Nothing below is generated on purpose. Until this is hand-written the kernel
; will not link (isr.c references the isrN/irqN symbols above).
