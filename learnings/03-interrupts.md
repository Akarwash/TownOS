# 3. Interrupts — The Heartbeat of an OS

**Source files:** `kernel/idt.c`, `kernel/idt.h`, `kernel/isr.c`, `kernel/isr.h`,
`kernel/isr_stubs.asm`

> **Note: this chapter describes the original 32-bit design.** MiniOS is now an
> x86-64 long-mode kernel. The concepts below still hold, but the mechanics have
> changed: gates are 16 bytes (not 8) and split the handler address three ways,
> the stubs push each register individually (there is no `pusha` in 64-bit) and
> return with `iretq`, and handlers receive `registers_t*` in `RDI`. For what
> actually runs today, see [../docs/reference/idt.md](../docs/reference/idt.md).
> This chapter is kept as personal learning material and is intentionally not
> rewritten.

If you learn only one thing about operating systems, learn this chapter.
Interrupts are the mechanism by which an OS stops being a program that *runs* and
becomes a program that *reacts*. Almost every other OS feature — multitasking,
device I/O, system calls, timers — is built on interrupts.

## The big idea

The CPU executes instructions one after another, forever. An **interrupt** is a
way to *forcibly divert* that execution to a special handler, then return exactly
where it left off — as if the diversion never happened.

Interrupts come in three flavors, and it is crucial to keep them straight:

1. **Exceptions (faults)** — the CPU interrupts *itself* because an instruction
   went wrong: divide by zero, invalid memory access (page fault), invalid opcode.
   Numbers **0–31** are reserved for these by Intel.
2. **Hardware interrupts (IRQs)** — a device (keyboard, timer, disk) raises a line
   to say "I need attention." These are *asynchronous*: they can happen between
   any two instructions.
3. **Software interrupts** — an instruction (`int N`) deliberately triggers an
   interrupt. This is the classic mechanism for **system calls** (e.g. Linux's old
   `int 0x80`).

All three funnel through the same machinery: a table of handlers indexed by
interrupt number. On x86 that table is the **Interrupt Descriptor Table (IDT)**.

## Why interrupts exist: the alternative is terrible

Imagine no interrupts. To know if a key was pressed, the CPU would have to
constantly *ask* the keyboard: "anything? anything? anything?" forever. This is
**polling**, and it wastes 100% of the CPU doing nothing useful. It also can't
react promptly to many devices at once.

Interrupts invert the relationship: the CPU does other work (or sleeps), and the
device *taps it on the shoulder* only when there is something to do. This is the
difference between refreshing your inbox every second and getting a notification.
It is what lets MiniOS spend 99.9% of its life asleep in a `hlt` loop
(chapter 6) yet respond instantly to a keypress.

## The IDT: a table of handlers

The IDT is an array of up to 256 **gate descriptors**. Entry `N` says: "when
interrupt number `N` fires, jump to *this* address, running in *this* code
segment, with *these* privileges." The layout mirrors the GDT idea from
chapter 2 — a table the hardware indexes into — but here it maps *interrupt
numbers* to *handler functions* instead of *selectors* to *memory regions*.

Open `idt.c`. The `idt_entry_t` struct is `__attribute__((packed))` and splits the
handler address into `base_low` / `base_high` (another historical layout). Each
entry also stores:

- a **selector** (`0x08`, the kernel code segment from chapter 2 — handlers run as
  kernel code), and
- **flags** `0x8E` = present, ring 0, "32-bit interrupt gate." An *interrupt gate*
  automatically disables further interrupts while the handler runs, so a handler
  is not itself interrupted mid-way (important for correctness).

`idt_init()` zeroes the whole table, remaps the PIC (below), and runs `lidt` to
load the IDT register — exactly the "table + pointer + special instruction"
pattern from the GDT.

## The PIC: routing hardware IRQs

Devices do not connect directly to the CPU's interrupt pin. They connect to a
**Programmable Interrupt Controller (PIC)** — on a classic PC, two 8259 chips
chained together, giving 15 usable IRQ lines. The PIC's job is to collect device
signals, prioritise them, and forward one at a time to the CPU.

### Why MiniOS *remaps* the PIC

Here is a classic PC design wart. By default, the PIC delivers its IRQs as
interrupt numbers **0–15**. But Intel reserved interrupt numbers **0–31** for CPU
exceptions! So by default a timer IRQ (line 0) arrives as interrupt 0 — which is
also "divide by zero." The kernel literally cannot tell a hardware timer tick from
a division error.

The fix is to **remap** the PIC so its 16 IRQs arrive as interrupt numbers
**32–47**, safely above the exception range. This is what `pic_remap()` in
`idt.c` does, by sending a specific four-step initialisation sequence (ICW1–ICW4)
to the PIC's I/O ports `0x20/0x21` (master) and `0xA0/0xA1` (slave):

```
IRQ 0  (timer)    → interrupt 32
IRQ 1  (keyboard) → interrupt 33
...
IRQ 15            → interrupt 47
```

This is why, throughout MiniOS, the timer registers its handler on **interrupt
32** and the keyboard on **interrupt 33**, *not* on 0 and 1. A very common bug
(called out in the build spec) is registering on the raw IRQ number and wondering
why nothing fires.

## From CPU to C: the ISR stubs

When an interrupt fires, the CPU does a few things automatically — pushes the
return address, code segment, and flags onto the stack, then jumps to the handler
address from the IDT. But it does **not** save the general-purpose registers, and
it does not tell your C code *which* interrupt fired. We have to bridge that gap in
assembly. That bridge is `isr_stubs.asm`.

The problem: a C function has a fixed signature, but there are 48 different
interrupts. We need 48 tiny assembly entry points that all funnel into one C
handler while recording which number fired. The stubs solve this.

### The error-code inconsistency

Another hardware wart: for a *few* exceptions (8, 10, 11, 12, 13, 14, 17, 21) the
CPU automatically pushes an **error code** onto the stack. For all other
interrupts it pushes nothing. If we did not normalise this, the stack layout would
differ between handlers and our `registers_t` struct would read garbage for half
of them.

The stubs fix it by having the *no-error-code* interrupts push a **dummy 0**, so
that **every** handler sees an identical stack layout:

```asm
%macro ISR_NOERR 1        ; exceptions without a CPU error code
isr%1:
    cli
    push dword 0          ; fake error code, keeps the layout uniform
    push dword %1         ; the interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1          ; exceptions where the CPU already pushed one
isr%1:
    cli
    push dword %1         ; just the interrupt number
    jmp isr_common_stub
%endmacro
```

Getting this wrong — pushing a dummy where the CPU already pushed a real error
code, or vice versa — shifts every field in `registers_t` by 4 bytes and the
handler reads nonsense. It is one of the most common "why did my kernel triple
fault" bugs.

### The common stub

All 48 stubs jump to a shared routine that:

1. `pusha` — saves all eight general-purpose registers.
2. saves and reloads the data segment registers to the kernel data selector
   (`0x10`), so the handler runs with known segments even if interrupted from
   elsewhere.
3. pushes `esp` (a pointer to everything just saved — this *is* the
   `registers_t*` the C handler receives) and `call`s the C handler.
4. restores everything, `add esp, 8` to discard the pushed interrupt number and
   error code, and finally `iret`.

`iret` ("interrupt return") is the counterpart to the CPU's automatic push: it
pops the flags, CS, and return address and resumes the interrupted code *exactly*
where it was. The interrupted program never knows it was paused.

### The `registers_t` struct

The fields of `registers_t` in `isr.h` are laid out in the **exact order** the
stub pushes them — `ds`, then the eight registers from `pusha`, then `int_no` and
`err_code`, then the five values the CPU pushed (`eip`, `cs`, `eflags`,
`user_esp`, `user_ss`). The struct is a C-shaped window onto the raw stack the
assembly built. This tight coupling between assembly and struct order is why the
two files must be edited together.

## The handler registry: the elegant part

`isr.c` keeps a single array:

```c
static isr_handler_t interrupt_handlers[256];
```

- `register_interrupt_handler(n, fn)` stores a function pointer at slot `n`.
- The C-side `isr_handler` / `irq_handler` look up `interrupt_handlers[int_no]`
  and call it if present.

This is a beautiful decoupling. The timer driver does not need to know anything
about the IDT or assembly stubs — it just calls
`register_interrupt_handler(32, timer_callback)`. The keyboard does
`register_interrupt_handler(33, keyboard_callback)`. New devices plug in the same
way. It is the same "dispatch table of callbacks" pattern you have seen in event
loops and GUI frameworks, here operating at the hardware level.

## EOI: telling the PIC "I'm done"

One rule you cannot forget for hardware IRQs: after handling one, you must send an
**End Of Interrupt (EOI)** signal to the PIC, or it will never deliver another
interrupt on that line — your keyboard goes dead after one keypress. `irq_handler`
does this:

```c
if (regs->int_no >= 40) {          // came from the slave PIC (IRQ 8-15)
    port_byte_out(0xA0, 0x20);     // EOI to slave
}
port_byte_out(0x20, 0x20);         // EOI to master (always)
```

The slave PIC is chained through the master, so IRQs 8–15 need *both* controllers
acknowledged. Exceptions (0–31) do **not** get an EOI, because they come from the
CPU, not the PIC — which is exactly why MiniOS has separate `isr_handler` and
`irq_handler` C functions.

## The order of operations (and why `sti` comes last)

`isr_install()` sets all 48 IDT entries, then calls `idt_init()`. Only *after* the
IDT is fully populated does the kernel run `sti` ("set interrupt flag") to enable
hardware interrupts. If you enabled interrupts before the handlers existed, the
first timer tick would jump to a garbage address and triple fault. **Build the
table, then open the door.**

## Going further

- **System calls**: the software-interrupt path. A user program puts a call number
  in a register and executes `int 0x80` (or the faster `syscall` instruction);
  the kernel's handler dispatches to the right function. It is the *same* IDT
  mechanism as a keyboard IRQ, just triggered by software from ring 3.
- **The APIC**: modern multicore machines replaced the 8259 PIC with the
  **APIC**/**IO-APIC**, which can route interrupts to specific CPU cores. The
  concept is unchanged; the routing is smarter.
- **Preemptive multitasking** falls right out of this chapter: register a handler
  on the timer interrupt that saves the current task's `registers_t` and loads
  another's. The timer tick becomes a "switch tasks now" signal. MiniOS's timer
  already ticks — turning it into a scheduler is the natural next project.
- **Top half / bottom half**: real drivers keep interrupt handlers tiny (just
  acknowledge and queue work) and defer the heavy lifting to a non-interrupt
  context, so interrupts stay disabled for as little time as possible.

### Exercises

1. Trace a single keypress from the physical key to `keyboard_callback`. Name every
   handoff: key → PIC → CPU → which IDT entry → which stub → which C function.
2. Why must `cli` happen at the start of each stub but `sti` (via `iret` restoring
   flags) at the end? What breaks if a handler is itself interrupted?
3. The build spec warns that forgetting EOI kills the keyboard after one press but
   asks you to send it *after* calling the handler. Does the order (EOI before vs.
   after the callback) matter here? When *would* it matter?
4. Interrupt 14 is the page fault, and it pushes an error code. Which stub macro
   handles it, and what does the error code tell you? (Preview of chapter 5.)
