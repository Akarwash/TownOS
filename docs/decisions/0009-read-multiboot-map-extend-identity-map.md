# 0009 - Read the Multiboot memory map and extend the identity map

## Status

Accepted.

## Context

Two memory sizes in the kernel were invented constants, not measured: the boot
identity map covered a fixed 8MB (`boot/boot.asm`), and the frame allocator's pool
was a hardcoded 128MB (`kernel/memory.c`). The two did not even agree, so the
allocator handed out addresses above 8MB that had no page-table entry and faulted
on first touch. `alloc_frame()` was therefore unusable in practice.

The information to fix this was already present and thrown away. The Multiboot
header sets `MBOOT_MEM_INFO`, so the bootloader builds a real memory map and passes
a pointer to it in EBX. But `kernel_main` took no arguments, so that pointer was
discarded at the handoff and the map was never read.

A ceiling had to be chosen. The single `pd_table` is one 4KB page: 512 entries of
2MB each, which reaches exactly 1GB. Covering more RAM would mean building
additional page directories, which is out of scope for a learning kernel.

## Decision

Read the Multiboot map and size both the identity map and the frame pool from it,
up to a 1GB ceiling.

- `boot/boot.asm` still builds a fixed identity map with no dependency on data it
  has not read yet, but enlarged from 8MB to 32MB (16 PD entries) to give C
  headroom. The 4-8M user region (PD[2]/PD[3]) keeps its `PG_USER` bit; 0-4M and
  8-32M are kernel-only. `pd_table` is exposed as `global`.
- The boot handoff forwards the EBX pointer to `kernel_main` in RDI (System V
  first argument). `kernel_main` now takes `uint64_t multiboot_info_addr`.
- `kernel/memory.c` parses the map, computes the highest usable (type 1) physical
  address, and fills `pd_table` entries from 32MB up to that address rounded to a
  2MB boundary, or to entry 511 (1GB), whichever is smaller. It then reloads CR3
  to flush stale TLB entries.
- The frame pool is sized from the same measured top of RAM (capped at 1GB), and
  every non-usable range the map reports is reserved by walking the map.
- If the map is absent (flags bit 6 clear), the kernel falls back to the fixed
  32MB the boot map already covers and prints a warning rather than reading
  garbage.

## Consequences

- `alloc_frame()` now returns real, mapped, writable RAM. The standing "allocator
  returns unmapped addresses" limitation is gone, and the invented-numbers problem
  with it.
- The 1GB cap is a real ceiling, set by the single PD page. RAM above 1GB is
  ignored; lifting the cap needs more page directories, which is deliberately not
  built here.
- The C extension edits the live page tables the CPU is currently walking. This is
  safe only because it touches high entries (32MB and up) exclusively and never
  the low entries that map the running kernel. Lowering that start index would
  move the ground under the CPU's own instruction fetch.
- The CR3 reload is mandatory and invisible: without it the newly written entries
  read back as not-present and fault. It is commented loudly where the extension
  loop lives.
- This is still a single shared address space with no per-process paging. Reading
  the map sizes memory correctly; it does not add isolation. That remains future
  work (see [0002](0002-2mb-pages-and-8mb-identity-map.md) and
  [../reference/memory-map.md](../reference/memory-map.md)).
- Under QEMU's built-in Multiboot loader the map is always present (the fallback
  path does not trigger), and the detected top of RAM tracks `-m` (about 127MB at
  128M, 255MB at 256M; the shortfall is the sub-1M and reserved holes the map
  excludes).
