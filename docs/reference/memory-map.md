# Physical memory map

This is the physical memory layout of TownOS as it stands, read from `linker.ld`,
`boot/boot.asm`, `kernel/gdt.c`, and `kernel/memory.c`. The kernel is identity
mapped: for the kernel's mapped region, virtual address equals physical address.
The one exception is the per-task user half: with per-process paging each task
maps the user virtual addresses (`0x400000` code, `0x800000` stack top) with 4KB
pages to its OWN physical frames, so there virtual address does NOT equal physical
address and differs per task. See [paging.md](paging.md).

## Fixed addresses

| Region | Address | Size | Source | Notes |
|--------|---------|------|--------|-------|
| Real-mode / low memory | `0x000000` | up to 1M | (hardware) | Not used by TownOS, but inside the identity map. |
| VGA text buffer | `0x0B8000` | 4000 bytes (80x25x2) | `drivers/screen.h` (`VIDEO_ADDRESS`) | Memory-mapped text output. Inside the identity map. |
| Kernel image load address | `0x100000` (1M) | image size | `linker.ld` (`. = 1M`) | Sections load here in order: `.multiboot`, `.text`, `.rodata`, `.data`, `.bss`. Ring-0 only (PD[0]/PD[1] have no user bit). |
| Ring-3 program code | `0x400000` (4M) | one small program per task | `user/user.ld`, `kernel/elf.c` | NOT part of the kernel image. Each program is a separate ELF64 file on the disk (`A.ELF`, `B.ELF`, `C.ELF`), linked at this address and loaded here at runtime. In the boot tree this range is a shared PD[2] huge page; with per-process paging each task gets PRIVATE 4KB pages at this same virtual address, holding its own loaded program (see below). |
| User stack (fixed virtual address, per-task frames) | `0x7C0000` - `0x7FFFFF` | 256 KB (`USER_STACK_SIZE`) | `kernel/scheduler.c` (`USER_STACK_TOP`, `USER_STACK_SIZE`, `USER_STACK_BASE`) | The top of PD[3]. Every task's stack sits at this SAME virtual address (top `0x800000`, growing down) but on its own physical frames in its own tree, so the stacks no longer share memory. This is a VIRTUAL address common to all trees, not a physical region carved up. |
| Boot identity map (fixed) | `0x000000` - `0x1FFFFFF` | 32 MB | `boot/boot.asm` (16 x 2MB PD entries) | Built by the boot climb with no dependency on the memory map. PD[0]/PD[1] and PD[4]/PD[15] kernel-only; PD[2]/PD[3] (4-8M) user-accessible. Covers the VGA buffer, the kernel, and the ring-3 pages, with headroom for C to extend. |
| Identity map extension (C) | `0x2000000` (32M) - top of RAM | up to 1 GB | `kernel/memory.c` (`memory_detect_and_map`) | Filled from the Multiboot map: entries from 32M up to the real top of usable RAM, rounded to 2MB, capped at 1GB (the single PD page's reach). Kernel-only. CR3 is reloaded afterwards to flush the TLB. |
| Frame allocator pool | `0x400000` (4M) - top of RAM | up to ~1 GB | `kernel/memory.c` (`MEMORY_START`, real top of RAM) | Bitmap tracks 4KB frames from 4M to the measured top of RAM (capped at 1GB). The 4-8M frames and every non-usable range the map reports are reserved at init, so the first free frame is real, mapped RAM. |

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

## How memory is sized: the boot map, the C extension, and the 1GB ceiling

Memory sizes are read from the machine, not invented. There are two stages.

**The fixed boot map (32MB).** The boot climb (`boot/boot.asm`) must build valid
page tables before it can enter long mode, and it has not read the memory map at
that point, so it maps a fixed, safe 32MB with 16 2MB PD entries. This is
deliberately larger than the kernel needs, to give the C code room to work in
before it extends the map. Only 4-8M (PD[2]/PD[3]) is user-accessible; the rest is
kernel-only.

**The C extension (up to real RAM, capped at 1GB).** Once in C,
`memory_detect_and_map()` (`kernel/memory.c`) reads the Multiboot map, finds the
highest usable (type 1) physical address, and fills `pd_table` entries from 32M up
to that address rounded to a 2MB boundary, or to entry 511, whichever is smaller.
The single `pd_table` is one 4KB page: 512 entries of 2MB reach exactly 1GB, so
1GB is a hard ceiling. RAM above 1GB is ignored; covering it would need more page
directories, which is out of scope. See
[decision 0009](../decisions/0009-read-multiboot-map-extend-identity-map.md).

**The CR3 flush is mandatory.** The CPU caches address translations in the TLB, so
the PD entries the extension writes do not take effect until the cache is
invalidated. `memory_detect_and_map()` reloads CR3 (`mov cr3, cr3`) after writing
them. This is invisible and easy to miss: skip it and the new high frames read
back as not-present and fault.

**The extension edits live page tables.** It is safe only because it touches high
entries (32M and up) exclusively, never the low entries that map the running
kernel. Modifying a low entry would move the ground under the CPU's own
instruction fetch.

**The mmap-entry stride gotcha.** Walking the map, the stride between entries is
`entry->size + sizeof(entry->size)`, not `sizeof(entry)`: the `size` field does
not count itself. Both walks in `memory.c` (top-of-RAM and reserved-range) advance
this way. Getting it wrong is a classic Multiboot bug that reads into garbage.

**The pool is sized to match.** The frame pool spans 4M to the measured top of RAM
(capped at 1GB). `memory_init()` reserves the 4-8M ring-3 region
(`USER_REGION_START` to `USER_REGION_END`, see
[decision 0006](../decisions/0006-user-mode-with-separate-pages.md)) and every
non-usable range the map reports, by walking the map. Because the pool never
exceeds what the identity map covers, the first free frame `alloc_frame()` returns
is real, mapped, writable RAM.

**No-map fallback.** If the bootloader provides no memory map (info flags bit 6
clear), the kernel does not read garbage: it falls back to the fixed 32MB the boot
map already covers and prints a warning. Under QEMU's built-in Multiboot loader
the map is always present, so this path does not trigger in practice.

## Per-task user half: same virtual addresses, private physical frames

The scheduler runs several ring-3 tasks (see [scheduling.md](scheduling.md)),
each with its own page-table tree (per-process paging). In the boot tree the user
region is two shared 2MB huge pages (PD[2] code, PD[3] stack), identity mapped.
Once a task is created it runs in ITS tree instead, where the user half is 4KB
pages to private frames:

- **Code** at `0x400000`: a private copy of the linked ring-3 image, one per
  task, on fresh frames.
- **Stack** at the top of PD[3] (`0x800000` top, `USER_STACK_SIZE` = 256KB down
  to `0x7C0000`): fresh frames, one set per task.

Every task uses the SAME virtual addresses but different physical frames, so the
old shared-stack-region hazard is gone: a task can no longer reach into another's
stack, because the two are simply not the same memory, and a stray pointer
outside the mapped code and stack faults instead of corrupting a neighbour. The
frames still cannot come from the kernel heap (`kmalloc` returns pages with no
PG_USER bit, which a ring-3 push would fault on); they come from `alloc_frame`
and are mapped user-accessible by `paging_map_page`. This retired the bump
allocator and the eight-stack ceiling from
[decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md). See
[paging.md](paging.md) and
[decision 0012](../decisions/0012-per-process-paging.md).

Note the physical frames backing these private mappings come from the frame pool
ABOVE 8M (the 4-8M region is reserved at init), even though the VIRTUAL addresses
are in the 4-8M user range.

**Where the bytes are copied from has changed.** The 4-8M identity mapping in the
boot tree used to be how the old `build_user_space` read the ring-3 image linked
into the kernel. There is no such image now: `task_create_from_file` loads a
program from a file, so `elf_load_file` copies out of a `kmalloc`'d buffer holding
the file's bytes into the freshly allocated frames, both reached through the
identity map of the regions they actually live in. The 4-8M mapping still exists
and still reserves those physical frames from the pool, but nothing reads a program
image through it any more.

## Related

- How the identity map is built: [boot-sequence.md](boot-sequence.md).
- The per-task page-table trees layered over this physical map:
  [paging.md](paging.md).
- Concepts behind physical vs virtual memory:
  [`../../learnings/05-memory-management.md`](../../learnings/05-memory-management.md).
