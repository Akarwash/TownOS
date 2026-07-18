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
| Frame allocator pool | `0x400000` (4M) - `0x8400000` (132M) | 128 MB | `kernel/memory.c` (`MEMORY_START`, `MAX_FRAMES`) | Bitmap tracks 32768 x 4KB frames starting at 4M. The 4-8M frames are reserved at init (see caveats below), so the first free frame is at 8M. |

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

## Caveat: the ring-3 region is reserved, and the pool is not usable yet

The frame allocator's pool starts at exactly `0x400000` (4M), the same address
where the ring-3 program's code and stack live (PD[2]/PD[3], 4-8M). To stop the
allocator from handing out frames that sit on top of the running user program,
`memory_init()` marks the frames covering 4-8M (`USER_REGION_START` to
`USER_REGION_END`) as used at init. See
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md).

Be clear about what this does and does not fix:

- The pool is 32768 frames of 4096 bytes, so it spans 4M to 132M.
- The identity map (`boot/boot.asm`) covers 8M. Every frame above 8M has no page
  table entry.
- After the reservation, the first free frame is at 8M, which is unmapped. So
  `alloc_frame()` returns an address that page-faults on first touch, every time.
- This change fixes "do not hand out frames something else is already using." It
  does not fix "the frames handed out are not mapped." The allocator remains
  unusable in practice until the identity map is extended.

Both the 8M identity map and the 128M pool are invented numbers, not measured.
The real fix is to read the Multiboot memory map (which the kernel currently
discards, since `kernel_main` takes no arguments) and size both from actual RAM.
That is future work, not a small oversight.

## Related

- How the identity map is built: [boot-sequence.md](boot-sequence.md).
- Concepts behind physical vs virtual memory:
  [`../../learnings/05-memory-management.md`](../../learnings/05-memory-management.md).
