# 0005 - A self-describing interrupt vector map

## Status

Accepted.

## Context

The CPU reserves interrupt vectors 0 to 31 for exceptions. Everything above that
is ours to assign. The universal convention, inherited from the PC/AT, is to
remap the 8259 PIC so hardware IRQs land at 0x20 to 0x2F and to put the Linux
syscall gate at 0x80.

Those numbers are not principled. 0x20 is simply the first multiple of 8 above
the reserved range, chosen in the early 1980s because it was the smallest legal
base. 0x80 is a historical accident of early Linux. Neither number tells you
anything about the interrupt when you see it in a log: 0x21 and 0x80 look alike,
yet one is a keyboard and the other is a program making a request. The category
has to be memorised, not read.

MiniOS is a teaching kernel. A vector number in a fault dump is something a
reader is meant to understand, not look up. The question was whether to follow
the convention or to pick numbers that carry their own meaning.

A PIC base is not free to be any number. The 8259's ICW2 latches only the upper
five bits of the base and substitutes the IRQ line into the low three, so a base
must be a multiple of 8 or several IRQs alias onto one vector. Any alternative
map has to respect that.

## Decision

Assign vectors by category, with the high nibble naming the category:

```
0x00 - 0x1F   CPU exceptions   Intel's, forced, cannot move
0x40 - 0x4F   hardware IRQs    "4 = the world poked us"
0x50 - 0x5F   syscalls         "5 = a program is asking for a favour"
```

The master PIC base is 0x40 and the slave base is 0x48, both multiples of 8, both
above the reserved range. So the timer (IRQ0) is 0x40, the keyboard (IRQ1) is
0x41, and IRQ8 to IRQ15 run 0x48 to 0x4F. 0x50 is reserved as `SYSCALL_VECTOR`
but nothing is wired to it yet.

Every vector number lives in one header, `include/vectors.h`, which is the single
source of truth. No bare vector number appears anywhere else in the tree. The one
unavoidable exception is `kernel/isr_stubs.asm`, which cannot include a C header
and so duplicates the master base as a NASM `equ`.

## Consequences

- A vector in a fault log now categorises itself. `0x4?` is a device, `0x5?` is a
  program asking, `0x0?` or `0x1?` is something breaking. This is the whole point.
- Every piece of external x86 material, the OSDev wiki, Stack Overflow answers,
  other kernels' source, assumes the 0x20 base. Anyone debugging MiniOS against an
  outside reference now has to translate: "their IRQ0 at 0x20 is our 0x40." That
  is a real, permanent tax on using the wider ecosystem, paid every time.
- `include/vectors.h` and the `equ` in `kernel/isr_stubs.asm` hold the same base
  with no compiler check that they agree. If one moves and the other does not,
  IRQs are installed at one set of gates and delivered to another, and the symptom
  is a dead keyboard or timer, not a build error. This is the same class of
  invisible coupling as the `registers_t` push order, and it is carried for the
  same reason: NASM cannot see the C header. Both sides carry a loud comment.
- The EOI logic had a hidden dependency on the old base: it acknowledged the slave
  PIC when a vector was 40 or higher, which was 32 + 8. Under the new base that
  test had to become "at or above the slave base (0x48)." Any future change to the
  base must keep that comparison symbolic, or IRQ8 to IRQ15 stop being acked.
- Choosing 0x40 leaves 0x20 to 0x3F unused, which is harmless but worth noting: we
  spent address space to buy legibility.

## Related

- The header itself: `include/vectors.h`.
- What is installed and why: [../reference/idt.md](../reference/idt.md).
- The concept chapter (still using the old 32 to 47 numbering, with a pointer
  here): [../../learnings/03-interrupts.md](../../learnings/03-interrupts.md).
