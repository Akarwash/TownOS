# IDT and interrupt entry reference

MiniOS installs a 256-entry **Interrupt Descriptor Table** so the CPU has a
handler for every exception and hardware interrupt. The table is built in
`kernel/idt.c`, the per-vector assembly entry points are in
`kernel/isr_stubs.asm`, and the C-side dispatch is in `kernel/isr.c`. This page
documents what is actually installed and why.

## Install order

`kernel_main` (`kernel/kernel.c`) calls `gdt_init()` first, then `isr_install()`
(`kernel/isr.c`). `isr_install()`:

1. calls `idt_init()` (`kernel/idt.c`), which zeroes the table, remaps the PIC,
   and loads the IDT register with `lidt`,
2. installs all 48 gates with `idt_set_entry()`,
3. runs `sti` to enable hardware interrupts.

The GDT must be installed before the IDT: a gate names a code selector, and the
CPU resolves that selector against the current GDT the instant an interrupt
fires. Interrupts are enabled last, only after every gate is populated, so the
first timer tick cannot jump to an empty gate.

## Vector table

| vectors | source | meaning |
|---------|--------|---------|
| 0 to 31 | CPU | exceptions (divide error, page fault, general protection, ...) reserved by Intel |
| 32 to 47 | PIC | hardware IRQs 0 to 15, remapped off the reserved range |
| 48 to 255 | unused | present in the table but zeroed; no handler installed |

IRQ remapping matters. By default the 8259 PIC delivers its IRQs as interrupt
numbers 0 to 15, which collide with the CPU exception range. `pic_remap()` in
`kernel/idt.c` reprograms the master and slave PICs so IRQ 0 arrives as vector
32, IRQ 1 as 33, and so on up to IRQ 15 at vector 47. This is why the timer
registers on vector 32 and the keyboard on vector 33 (`kernel/timer.c`,
`drivers/keyboard.c`), not on 0 and 1.

## Gate format (16 bytes)

A long-mode gate descriptor is 16 bytes, double the 8-byte protected-mode gate.
The handler address is scattered across three fields (`idt_entry_t` in
`kernel/idt.h`):

| field | width | contents |
|-------|-------|----------|
| `offset_low` | u16 | handler address bits 0 to 15 |
| `selector` | u16 | code segment selector (`0x08`, kernel code) |
| `ist` | u8 | Interrupt Stack Table index (bits 0 to 2), 0 here |
| `flags` | u8 | present, DPL, and gate type (`0x8E`) |
| `offset_mid` | u16 | handler address bits 16 to 31 |
| `offset_high` | u32 | handler address bits 32 to 63 |
| `zero` | u32 | reserved, must be 0 |

`idt_set_entry()` packs a 64-bit handler address into `offset_low` /
`offset_mid` / `offset_high`, sets the selector and flags, and zeroes `ist` and
`zero`. `ist = 0` means "keep using the current stack" rather than switching to a
preset IST stack; MiniOS never crosses privilege levels, so the running stack is
always a valid kernel stack.

## The flags byte: `0x8E`, an interrupt gate

Every gate uses `flags = 0x8E` = present (bit 7), DPL 0 (bits 6 to 5), and gate
type `0xE` (a 64-bit **interrupt gate**).

**Interrupt gate vs trap gate.** The two gate types differ in one behavior: an
interrupt gate clears the interrupt flag (IF) on entry, so the handler runs with
further hardware interrupts masked and cannot be nested by another IRQ. A trap
gate (type `0xF`) leaves IF as it was, allowing nesting. MiniOS uses interrupt
gates everywhere so a handler is never interrupted mid-way, which keeps the
handler and the shared dispatch state simple. The counterpart `iretq` at the end
of each stub restores the saved flags, re-enabling interrupts on return.

## Why every gate is DPL 0

The DPL in the flags byte is the lowest privilege level allowed to reach the gate
through a software `int N` instruction. All 48 gates are DPL 0. A DPL 3 gate
would let ring-3 user code execute, say, `int 14` to forge a page fault or
`int 32` to fake a timer tick, corrupting kernel state at will. Hardware
interrupts and CPU exceptions ignore the DPL, so DPL 0 costs nothing. Only a
future syscall vector would deliberately want DPL 3, so user code could enter the
kernel through that one gate on purpose.

## Error-code vectors

For a few exceptions the CPU automatically pushes an error code onto the stack
before entering the handler; for all other interrupts it pushes nothing. The
error-code vectors are **8, 10, 11, 12, 13, 14, 17**. To give the C handler one
uniform stack layout, the assembly stubs push a dummy 0 in the error-code slot
for every vector that does not get a real one (the `ISR_NOERR` macro), while the
error-code vectors push only the vector number (the `ISR_ERR` macro). Hardware
IRQs never carry an error code, so they always push a dummy. This uniformity is
what lets `registers_t` (`kernel/isr.h`) describe every interrupt's stack frame
identically.

## Why 48 separate entry points exist

The CPU signals which interrupt fired only by which address it jumps to; there is
no register that reports the vector number. So `kernel/isr_stubs.asm` defines one
tiny entry point per vector (`isr0` to `isr31`, `irq0` to `irq15`), and each one
hardcodes and pushes its own number before jumping to a shared stub. The shared
stub then:

1. pushes all 15 general-purpose registers (there is no `pusha` in 64-bit),
2. loads the stack pointer into `RDI`, which is the `registers_t*` first argument
   under the System V AMD64 calling convention,
3. calls the C handler (`isr_handler` for exceptions, `irq_handler` for IRQs),
4. pops the registers, drops the pushed vector number and error code with
   `add rsp, 16`, and returns with `iretq`.

The register push order is the exact reverse of the field order in `registers_t`,
so that after the final `push rdi` the stack pointer is a valid `registers_t*`.
This coupling between the assembly and the struct is invisible to the compiler,
so both `kernel/isr_stubs.asm` and `kernel/isr.h` carry a comment warning that
they must change together.

## Dispatch and EOI

`isr.c` keeps a `interrupt_handlers[256]` table of callbacks.
`register_interrupt_handler(n, fn)` installs one; `isr_handler` / `irq_handler`
look up the slot and call it. A driver never touches the IDT or the stubs; the
timer just calls `register_interrupt_handler(32, ...)` and the keyboard
`register_interrupt_handler(33, ...)`.

Hardware IRQs additionally require an **End Of Interrupt** signal to the PIC, or
that line goes dead after one interrupt. `irq_handler` sends EOI to the master
PIC always, and to the slave PIC as well when the vector is 40 or higher (IRQ 8
to 15 come through the slave). Exceptions get no EOI because they come from the
CPU, not the PIC, which is why `isr_handler` and `irq_handler` are separate.

## Related

- Concepts behind interrupts, the PIC, and EOI:
  [`../../learnings/03-interrupts.md`](../../learnings/03-interrupts.md) (note:
  that chapter describes the earlier 32-bit design).
- The GDT whose code selector the gates reference: [gdt.md](gdt.md).
- Boot climb that precedes IDT setup: [boot-sequence.md](boot-sequence.md).
