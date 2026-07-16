# 0004 - Build the TSS now, before user mode exists

## Status

Accepted.

## Context

The Task State Segment is only consulted by the CPU on a privilege-level change,
for example when an interrupt takes the CPU from ring 3 into ring 0 and it needs a
known-good kernel stack from `tss.rsp0`. MiniOS currently runs everything at CPL 0,
so no interrupt crosses a privilege boundary and the TSS is never actually read.
The TSS is therefore inert today. The question was whether to build it now or defer
it until user mode exists to exercise it.

## Decision

Implement the TSS now anyway: the 16-byte TSS descriptor, the `tss_t` struct, a
dedicated ring-0 stack, and the `ltr` that loads the task register, all as part of
`kernel/gdt.c`.

## Consequences

- There is currently untestable code in the tree: the TSS path runs but nothing
  yet depends on its effect.
- The payoff is that the fiddly part of `gdt.c` is the 16-byte descriptor format
  (a 64-bit base split across two GDT slots). Doing it once, together with the
  rest of the GDT, avoids coming back and touching this file a second time when
  user mode arrives.
- The ring-0 stack for the TSS is deliberately separate from the boot stack in
  `boot/boot.asm`, so that a future ring-3 to ring-0 transition switches to a
  clean, dedicated stack rather than reusing the boot stack. Sizes and locations
  are in [../reference/memory-map.md](../reference/memory-map.md).
- When user mode is added later, the stack-switch mechanism is already in place
  and only needs to be exercised, not built.
