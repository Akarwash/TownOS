# 0012 - Per-process paging: a private address space per task

## Status

Accepted. Partly superseded — the decision stands, two details of the body do not.

- **How the user half is filled** was superseded by
  [0015](0015-elf-program-loading.md). The body describes `task_create` copying
  the whole linked ring-3 image (`_user_text_start` to `_user_rodata_end`, all
  three programs) into every task. Programs are now separate ELF files and
  `elf_load_file` maps one program's own `PT_LOAD` segments.
- **Three tasks at boot** was superseded by
  [0016](0016-interactive-shell.md). `kernel_main` starts `SHELL.ELF` alone;
  everything else is launched by `run`.

The private tree per task, the by-value kernel-half clone, the 4KB/2MB split and
the CR3 switch are all unchanged and still describe the code. See
[reference/paging.md](../reference/paging.md) for the current state.

## Context

Until now every task ran in the single boot page-table tree (`boot/boot.asm`):
one PML4, one PDPT, one PD, all 2MB huge pages, identity mapped, shared by the
kernel and every ring-3 task alike. That has two consequences the earlier
decisions kept flagging as future work:

- **No isolation.** Because virtual address equals physical address for
  everyone, two tasks cannot use the same virtual address for different memory.
  Any task can read or write any other task's memory, and there is nothing to
  fault on a stray pointer.
- **One shared user-stack region.** [Decision 0011](0011-dynamic-tasks-and-stacks.md)
  bump-allocated 256KB stack slices out of the one PG_USER region (PD[3],
  `0x600000`-`0x800000`), so there were only eight stacks and no guard pages: a
  task that overran its slice scribbled into a neighbour's. Both 0011 and
  [0008](0008-round-robin-preemptive-scheduler.md) named per-process paging as
  the only real fix, and 0011 left a `TODO(per-process-paging)` at the site.

The heap ([0010](0010-kernel-heap-ported-from-p5.md)) and the real frame pool
([0009](0009-read-multiboot-map-extend-identity-map.md)) that this needs now
exist, so the address-space work is unblocked.

The goal is a private page-table tree per task, loaded into CR3 on every context
switch, so two tasks can use the *same* virtual address (both run their code at
`0x400000`, both put their stack top at `0x800000`) and land on *different*
physical memory. That difference is the isolation.

## Decision

Give each task its own tree with two halves, and switch CR3 in the scheduler.

### Two halves: private user, shared kernel

Every tree is split the way every real kernel splits an address space:

- **User half, private.** 4KB pages to freshly allocated frames, user bit set.
  This is what makes `0x400000` mean different physical memory in different
  trees. It MUST be 4KB pages: a 2MB huge-page identity mapping forces
  virtual==physical, which cannot give two tasks different physical memory at the
  same virtual address. So each tree is deliberately MIXED page sizes: 4KB for
  the private user region, 2MB huge pages for the shared kernel region.
- **Kernel half, identical in every tree, kernel-only.** The kernel must be
  mapped in every tree because when an interrupt fires the CPU jumps into kernel
  code WITHOUT changing CR3: the first kernel instruction is fetched through
  whatever task tree is currently loaded. If the kernel were not mapped there,
  the switch itself would triple-fault. The IDT, GDT, TSS `rsp0` stack, the
  interrupt stub, the kernel stack the interrupt frame sits on, and all kernel
  code and data therefore live in the kernel half of every tree.

### Option A: clone the kernel half BY VALUE

The natural first instinct is to share the kernel half BY REFERENCE: copy the
boot `PML4[0]` entry so every tree's `PML4[0]` points at the one boot
`pdpt_table`/`pd_table`, and the kernel is mapped everywhere for free. **That
does not fit this kernel's layout.** Everything hangs off a single branch:
`PML4[0] -> PDPT[0] -> pd_table`, and the ring-3 region lives INSIDE that same
`pd_table` (the user code is `pd_table[2]`, the user stack `pd_table[3]`).
Sharing `pd_table` by reference would share the user huge pages too, making a
private 4KB mapping at `0x400000` impossible: the whole point.

So MiniOS uses **Option A**: each task gets its OWN PML4, PDPT, and PD for the
low 1GB. The PD is filled by COPYING the boot PD entries by value, every entry
except the two user slots (`pd_table[2]`/`pd_table[3]`, left absent so the user
branch can be overridden with private 4KB page tables). Each copied kernel entry
carries the identical 2MB huge-page mapping (same physical kernel frame, no user
bit), so every tree maps the identical kernel at the identical addresses,
kernel-only, exactly as the boot tree did.

`paging_create_address_space()` (`kernel/paging.c`) builds this; `paging_map_page()`
installs the private 4KB user mappings; `paging_switch()` loads CR3.

### Copy the user image per task

`task_create` (`kernel/scheduler.c`) copies the WHOLE linked ring-3 image
(`_user_text_start` to `_user_rodata_end`, all three programs) into fresh frames
mapped at its link address `0x400000`, and maps a fresh stack at a fixed virtual
address (top `0x800000`) that every task reuses. The user code stays at
`0x400000`, so nothing is relinked and each task's forged `rip` (its own
function's linked address) stays valid.

Copying the full image per task is the STRONGEST isolation but the most wasteful:
the read-only text is duplicated three times over. This is a deliberate teaching
choice (one obvious mechanism, no shared-frame bookkeeping to reason about),
marked `TODO(shared-text)` at the copy site for the cheaper by-reference-text,
copy-only-data refinement.

### The frozen-kernel-mappings tripwire

By-value cloning is correct ONLY because kernel mappings are FROZEN after boot.
`memory_detect_and_map` fills the identity map once at startup, before any task
tree exists, and nothing ever mutates a kernel PD entry afterward, so a by-value
copy can never drift out of sync with the boot tables. This is a load-bearing
invariant, not an incidental fact:

> If the kernel ever remaps itself at runtime (kernel ASLR, memory hot-plug, a
> higher-half relocation), the by-value copies already living in every task tree
> would silently go stale, and the kernel would see different mappings depending
> on which task was current. At that point Option A MUST become a by-reference
> share of the kernel tables, or gain an explicit propagation step that updates
> every existing tree.

The tripwire is recorded three ways so it cannot be missed: in a comment at the
`extern pd_table` clone site in `kernel/paging.c`, in
[reference/paging.md](../reference/paging.md), and here.

## Consequences

- **Tasks are isolated.** The three programs run at identical virtual addresses
  (code `0x400000`, stack top `0x800000`) backed by distinct physical frames per
  task. A stray or overflowing pointer now faults (the user branch is absent
  outside the mapped code and stack) instead of silently corrupting a neighbour,
  the guard the old shared layout never had.

- **The user-stack ceiling from 0011 is gone.** Stacks no longer compete for one
  shared 2MB region; each task's stack is private, at the same VA on its own
  frames. The bump allocator (`alloc_user_stack`, `USER_STACK_REGION_*`) is
  removed.

- **Mixed page sizes in one tree.** Each tree is 4KB pages for the private user
  region and 2MB huge pages for the shared kernel region. The page walk
  terminates at the PD (PG_HUGE) for kernel addresses and descends to a PT for
  user addresses.

- **The CR3 switch is safe mid-interrupt, by construction.** `schedule()` loads
  the incoming task's CR3 after copying its register pile; `scheduler_start()`
  loads task 0's CR3 before the first drop to ring 3. This is safe only because
  everything the CPU still needs on the way out (the register pile `r` on the
  kernel stack, the `tasks[]` array and scheduler code in kernel `.data`/`.text`,
  and the IDT/GDT/TSS/stub the next timer tick will reach) lives in the kernel
  half, cloned identically into every tree. The switch changes only the user
  half; the kernel never disappears out from under itself. Writing CR3 also
  flushes the TLB (no global pages are used), which drops the previous task's
  stale user translations for free.

- **Page tables come from `alloc_frame`, not `kmalloc`.** Page-table pages must
  be 4KB and 4KB-aligned, which is exactly what `alloc_frame` hands out; `kmalloc`
  carves sub-page slab pieces and does not align to a frame. Only the small
  `address_space_t` handle (kernel-only bookkeeping) goes on the heap. Every
  page-table frame lives inside the identity-mapped pool, so its physical address
  is also a writable virtual pointer, which is how the tables are filled.

- **The user bit is set at every level.** x86 permits a ring-3 access only if the
  user bit is set at EVERY level of the walk (the AND-down rule). The intermediate
  PML4/PDPT/PD entries on the user branch are made user-permissive and the actual
  privilege is gated at the leaf PTE, so ring 3 still cannot reach the kernel
  leaves even though the branch above them says "user may pass".

- **Memory is wasted by the per-task text copy.** Three private copies of the
  read-only user image. Acceptable for a teaching kernel with three tasks;
  `TODO(shared-text)` marks the refinement.

- **Verified under QEMU.** With `-d int` the run shows three distinct task CR3
  values, the same three user RIPs (`0x40002b`, `0x400083`, `0x4000db`), an even
  round-robin interleave, and zero page (`0x0E`), GP (`0x0D`), or double (`0x08`)
  faults. A temporary isolation proof (removed after) walked each task's tree and
  confirmed the shared VAs `0x400000` and the stack top page resolve to different
  physical frames in all three trees.

## Related

- The shared-region layout and stack ceiling this lifts:
  [0011](0011-dynamic-tasks-and-stacks.md) and
  [0006](0006-user-mode-with-separate-pages.md).
- The scheduler whose switch now also loads CR3:
  [0008](0008-round-robin-preemptive-scheduler.md).
- The frozen kernel identity map the by-value clone depends on:
  [0009](0009-read-multiboot-map-extend-identity-map.md).
- The heap the `address_space_t` handle lives on:
  [0010](0010-kernel-heap-ported-from-p5.md).
- Reference pages: [../reference/paging.md](../reference/paging.md),
  [../reference/scheduling.md](../reference/scheduling.md), and
  [../reference/memory-map.md](../reference/memory-map.md).
</content>
</invoke>
