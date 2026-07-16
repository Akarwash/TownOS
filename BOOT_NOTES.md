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
