# MiniOS boot notes — the 32 → 64 long-mode climb

`boot/boot.asm` receives control from GRUB/QEMU in **32-bit protected mode with
paging off**. Long mode requires paging on, so the climb builds valid page
tables first and only then flips the switch. All code before the far jump runs
under `[bits 32]`; everything after runs under `[bits 64]`.

## The climb, step by step

1. **Zero the tables.** Clear all 12 KB (PML4 + PDPT + PD) by hand — the
   bootloader is not trusted to zero `.bss`, and one stray present bit is a
   silent triple fault.
2. **PML4[0] → PDPT.** Top-level entry points at the next level, present + writable.
3. **PDPT[0] → PD.** Second-level entry points at the page directory.
4. **PD[0..3] = identity map.** Four 2MB pages cover 0x000000–0x7FFFFF (VGA text
   buffer at 0xB8000 and the kernel at 1M). The `PS`/huge bit (bit 7) is
   mandatory: it stops the walk at the PD instead of chasing a nonexistent PT.
5. **Enable PAE.** Set CR4 bit 5 — required for long mode.
6. **Load CR3.** Point it at the physical base of the PML4 (identity-mapped, so
   the label address is the physical address).
7. **Set EFER.LME.** `rdmsr` MSR `0xC0000080`, set bit 8, `wrmsr`. This only arms
   long mode.
8. **Enable paging (CR0.PG).** Setting bit 31 activates long mode; address
   translation now applies to our own instruction fetches — which is why step 4
   had to be correct first.
9. **Load the bootstrap GDT, far-jump into 64-bit code.** The far jump reloads CS
   with a 64-bit code selector — the only way to reach true 64-bit execution.
10. **64-bit code.** Load the flat data selector into the segment registers, set
    `RSP = stack_top`, and `call kernel_main` (which takes no arguments).
11. **Halt.** `kernel_main` should never return; if it does, `cli` + `hlt` loop.

## Bootstrap GDT vs `kernel/gdt.c`

The GDT at the bottom of `boot.asm` is a **bootstrap GDT only**: a null
descriptor plus one 64-bit code and one flat data descriptor. Its sole purpose is
to make the far jump in step 9 legal. It is **separate** from `kernel/gdt.c`,
which installs the real kernel GDT once C is running. The two are intentionally
not shared or unified — the bootstrap GDT just gets us into 64-bit mode, after
which the C code replaces it.

## If it triple-faults when this eventually runs

Debug with:

```bash
qemu-system-x86_64 -kernel minios.bin -d int -no-reboot -no-shutdown
```

Likely suspects:

- a missing `PS` (huge) bit on a PD entry,
- a page table that was not 4096-aligned,
- a table that was not zeroed before use, or
- an identity map that does not cover the address of the code executing at the
  moment CR0.PG is set.

# The kernel GDT + TSS (`kernel/gdt.c`, `gdt.h`, `gdt_flush.asm`)

`boot.asm` gets us into long mode with a throwaway bootstrap GDT. Once C is
running, `gdt_init()` installs the real kernel GDT. It deliberately keeps the
bootstrap selectors (`0x08` kernel code, `0x10` kernel data) meaning the same
thing, so the CS reload inside `gdt_flush` stays valid.

## Selector layout (7 slots)

| index | descriptor | selector |
|---|---|---|
| 0 | null | 0x00 |
| 1 | kernel code, DPL 0 | 0x08 |
| 2 | kernel data, DPL 0 | 0x10 |
| 3 | user code, DPL 3 | 0x1B (0x18 \| 3) |
| 4 | user data, DPL 3 | 0x23 (0x20 \| 3) |
| 5, 6 | TSS descriptor (16 bytes, spans two slots) | 0x28 |

## Why the TSS descriptor is 16 bytes, not 8

Code/data descriptors are 8 bytes with a 32-bit base — fine, because their base
is ignored in long mode anyway. A TSS descriptor, though, must point at a real
64-bit linear address, and a 64-bit base does not fit in the legacy 8-byte
descriptor's 32-bit base fields. So a system (TSS) descriptor is extended with an
extra 32 bits of base plus a reserved word, making it 16 bytes — it occupies two
GDT slots (5 and 6 here).

## Why CS is reloaded with `retfq`, not a far jump

In 32-bit mode you reload CS with `jmp 0x08:label`. In 64-bit mode that
encoding — a far jump with an immediate `selector:offset` — does not exist. The
workaround is a far *return*: `retfq` pops RIP and then CS off the stack. So
`gdt_flush` pops its own return address, pushes the new CS selector (`0x08`),
pushes the return address back on top, and `retfq`s — which lands back at the
caller with CS reloaded. The data segments (`ds/es/fs/gs/ss`) are ordinary `mov`s
and need no such trick.

## The TSS is inert for now

`ltr 0x28` loads the task register, and `tss.rsp0` points at a dedicated 16KB
ring-0 stack (separate from the boot stack). But everything currently runs at
CPL 0, so no interrupt crosses a privilege boundary, so the CPU never consults
`rsp0`. The TSS exists purely for the user-mode (ring 3) work coming later.
