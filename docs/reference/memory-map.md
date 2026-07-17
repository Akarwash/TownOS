# Physical memory map

This is the physical memory layout of MiniOS as it stands, read from `linker.ld`,
`boot/boot.asm`, `kernel/gdt.c`, and `kernel/memory.c`. There is no virtual
remapping beyond the identity map: for the mapped region, virtual address equals
physical address.

## Fixed addresses

| Region | Address | Size | Source | Notes |
|--------|---------|------|--------|-------|
| Real-mode / low memory | `0x000000` | up to 1M | (hardware) | Not used by MiniOS, but inside the identity map. |
| VGA text buffer | `0x0B8000` | 4000 bytes (80x25x2) | `drivers/screen.h` (`VIDEO_ADDRESS`) | Memory-mapped text output. Inside the identity map. |
| Kernel image load address | `0x100000` (1M) | image size | `linker.ld` (`. = 1M`) | Sections load here in order: `.multiboot`, `.text`, `.rodata`, `.data`, `.bss`. Ring-0 only (PD[0]/PD[1] have no user bit). |
| Ring-3 program code (`.user_text`) | `0x400000` (4M) | ~29 bytes | `linker.ld`, `user/user_program.c` | User-accessible (PD[2], user bit set). Its own `PT_LOAD` segment so the gap from the kernel is not padded into the file. |
| Ring-3 program stack | `0x600000` - `0x7FFFFF` | 2 MB page | `boot/boot.asm` (PD[3]), `USER_STACK_TOP` = `0x800000` | User-accessible (PD[3], user bit set). Stack grows down from `0x800000`. |
| Identity-mapped region | `0x000000` - `0x7FFFFF` | 8 MB | `boot/boot.asm` (4 x 2MB PD entries) | The only mapped region. PD[0]/PD[1] kernel-only; PD[2]/PD[3] user-accessible. Covers the VGA buffer, the kernel, and the ring-3 pages. |
| Frame allocator pool | `0x400000` (4M) - `0x8400000` (132M) | 128 MB | `kernel/memory.c` (`MEMORY_START`, `MAX_FRAMES`) | Bitmap tracks 32768 x 4KB frames starting at 4M. Overlaps the ring-3 region — see caveats below. |

## Objects in `.bss` (addresses determined at link time)

These live in `.bss`, above the kernel image at 1M. Their exact addresses depend
on the linked image and are not fixed constants (and the image does not link yet),
so only sizes and alignment are stated here.

| Object | Size | Alignment | Source |
|--------|------|-----------|--------|
| Boot stack (`stack_bottom`..`stack_top`) | 16 KB (16384 bytes) | 16 | `boot/boot.asm` |
| PML4 table | 4 KB | 4096 | `boot/boot.asm` |
| PDPT table | 4 KB | 4096 | `boot/boot.asm` |
| PD table | 4 KB | 4096 | `boot/boot.asm` |
| TSS ring-0 stack (`tss_stack`) | 16 KB (16384 bytes) | 16 | `kernel/gdt.c` |
| Frame bitmap | 4 KB (32768 bits) | default | `kernel/memory.c` |

The three page tables are contiguous and each is exactly one 4KB page, 4096-byte
aligned because the CPU ignores the low 12 bits of a page-table base address.

The TSS ring-0 stack is deliberately separate from the boot stack: it is the stack
the CPU switches to on a ring-3 to ring-0 transition (`tss.rsp0` points at its
top). See [gdt.md](gdt.md) and
[decision 0004](../decisions/0004-build-tss-before-user-mode.md).

## Caveat: the frame pool extends past the identity map

The bitmap frame allocator (`kernel/memory.c`) hands out physical addresses from
4M up to 132M, but only the first 8M is identity-mapped by the boot page tables.
The allocator only returns addresses and does not itself touch that memory, so
this is not a bug today, but any code that actually dereferences a frame above 8M
would fault until a real virtual-memory system maps it. Per-process address spaces
and a proper VM layer are future work (see
[decision 0002](../decisions/0002-2mb-pages-and-8mb-identity-map.md)).

## Caveat: the frame pool overlaps the ring-3 region

The frame allocator's pool starts at exactly `0x400000` (4M) — the same address
where the ring-3 program's code and stack live (PD[2]/PD[3], 4-8M). So the
allocator would hand out frames that sit on top of the running user program. This
is latent, not active: in the current demo `kernel_main` drops to ring 3
immediately after `memory_init()` and nothing allocates a frame, and the
allocator only returns addresses without writing them. A real user/VM layer must
resolve the collision (move the pool, or reserve the user region in the bitmap).
See [decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

## Related

- How the identity map is built: [boot-sequence.md](boot-sequence.md).
- Concepts behind physical vs virtual memory:
  [`../../learnings/05-memory-management.md`](../../learnings/05-memory-management.md).
