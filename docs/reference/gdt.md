# GDT and TSS reference

MiniOS uses two GDTs in sequence. `boot/boot.asm` installs a minimal **bootstrap
GDT** to make the far jump into 64-bit code legal. Once C is running,
`gdt_init()` (`kernel/gdt.c`) installs the real **kernel GDT** and loads the task
register. This page documents the kernel GDT; for the bootstrap GDT see the note
at the end and [decision 0003](../decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md).

## Selector layout (7 slots)

| index | descriptor | selector |
|-------|------------|----------|
| 0 | null | `0x00` |
| 1 | kernel code, DPL 0 | `0x08` |
| 2 | kernel data, DPL 0 | `0x10` |
| 3 | user code, DPL 3 | `0x1B` (`0x18 \| 3`) |
| 4 | user data, DPL 3 | `0x23` (`0x20 \| 3`) |
| 5, 6 | TSS descriptor (16 bytes, spans two slots) | `0x28` |

The kernel GDT deliberately keeps kernel code at `0x08` and kernel data at `0x10`,
the same selectors the bootstrap GDT uses. If these changed meaning between
`lgdt` and the CS reload, the far return inside `gdt_flush` would fault.

## Descriptor bit layout (8-byte code/data)

Each ordinary descriptor is 8 bytes: `limit_low` (u16), `base_low` (u16),
`base_middle` (u8), `access` (u8), `granularity` (u8, limit high nibble + flags
high nibble), `base_high` (u8).

**Access byte** (bit 7 to 0): `P | DPL(2) | S | E | DC | RW | A`. Assembled values:

| descriptor | access | meaning |
|------------|--------|---------|
| kernel code | `0x9A` | P=1, DPL=0, S=1, E=1, RW=1 |
| kernel data | `0x92` | P=1, DPL=0, S=1, E=0, RW=1 |
| user code | `0xFA` | P=1, DPL=3, S=1, E=1, RW=1 |
| user data | `0xF2` | P=1, DPL=3, S=1, E=0, RW=1 |
| TSS | `0x89` | P=1, DPL=0, S=0, type=9 (available 64-bit TSS) |

**Flags nibble** (granularity high nibble, bit 7 to 4): `G | D/B | L | AVL`. A
64-bit code segment sets `L=1` and clears `D/B` (setting both is an illegal
combination). Data segments carry no meaningful flags in long mode, so their
nibble is 0.

## Which fields long mode ignores, and which still matter

For code and data descriptors, long mode **ignores base and limit**: memory
protection is done by paging, not segmentation. Those fields still exist because
the descriptor format is fixed by hardware, but the CPU does not use them. What
still matters for code/data is the access byte (present, DPL, executable) and the
`L` flag that marks a code segment as 64-bit.

For the **TSS descriptor**, base and limit **do** matter: the base is the linear
address of the TSS, and the limit is its size.

## Why the TSS descriptor is 16 bytes, not 8

A code/data descriptor's base is a 32-bit field, which is fine because that base is
ignored anyway. The TSS descriptor must point at a real 64-bit linear address, and
a 64-bit base does not fit in the legacy 8-byte descriptor's 32-bit base fields. So
a 64-bit system (TSS) descriptor is extended with an extra 32 base bits plus a
reserved word, making it 16 bytes. It therefore occupies two GDT slots (5 and 6
here). In `kernel/gdt.h` this is `tss_descriptor_t`: a `gdt_entry_t` followed by
`base_upper` (u32) and `reserved` (u32).

## TSS struct layout (104 bytes)

The 64-bit TSS (`tss_t` in `kernel/gdt.h`) no longer holds a task context; it
mainly provides the stack pointers the CPU switches to on a privilege-level
change. Layout, in order:

- `reserved0` (u32)
- `rsp0`, `rsp1`, `rsp2` (u64 each): stacks for entry into rings 0, 1, 2
- `reserved1` (u64)
- `ist1`..`ist7` (u64 each): interrupt stack table entries (unused)
- `reserved2` (u64)
- `reserved3` (u16)
- `iomap_base` (u16): offset to the I/O permission bitmap

`sizeof(tss_t)` is asserted to be exactly 104 bytes with a `_Static_assert`, to
catch packing mistakes. `gdt_init()` sets `tss.rsp0` to the top of a dedicated
16KB ring-0 stack and `tss.iomap_base = sizeof(tss_t)` (meaning "no I/O bitmap"),
zeroes the rest, then loads the task register with `ltr 0x28`.

The TSS is inert today: everything runs at CPL 0, so no interrupt crosses a
privilege boundary and the CPU never consults `rsp0`. It exists for the user-mode
(ring 3) work to come. See
[decision 0004](../decisions/0004-build-tss-before-user-mode.md).

## Reloading CS: far return, not far jump

`gdt_flush` (`kernel/gdt_flush.asm`) takes the `gdt_ptr` address in RDI, runs
`lgdt [rdi]`, reloads `ds/es/fs/gs/ss` with `0x10` (ordinary `mov`s), then reloads
CS. In 32-bit mode you reload CS with `jmp 0x08:label`; in 64-bit mode that
encoding (a far jump with an immediate `selector:offset`) does not exist. The
workaround is a far return: `retfq` pops RIP and then CS off the stack. So
`gdt_flush` pops its own return address, pushes the new CS selector (`0x08`),
pushes the return address back on top, and executes `retfq`, which lands back at
the caller with CS reloaded.

## The bootstrap GDT in `boot.asm`

`boot/boot.asm` contains a separate, minimal GDT: a null descriptor, one 64-bit
code descriptor, and one flat data descriptor. Its only job is to make the far
jump into 64-bit code legal during boot. It is intentionally not shared with the
kernel GDT; the kernel GDT replaces it at runtime. The two are kept separate on
purpose, documented in
[decision 0003](../decisions/0003-bootstrap-gdt-separate-from-kernel-gdt.md).

## Related

- Boot climb that uses the bootstrap GDT: [boot-sequence.md](boot-sequence.md).
- Concepts behind segmentation and the GDT:
  [`../../learnings/02-protected-mode-and-the-gdt.md`](../../learnings/02-protected-mode-and-the-gdt.md).
