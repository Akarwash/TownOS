# Per-process paging reference

Each task in MiniOS runs in its own page-table tree, loaded into CR3 on every
context switch, so two tasks can use the same virtual address for different
physical memory. This page documents how a tree is built, why the kernel is
mapped in every one, the 4KB/2MB split, and the one invariant the whole scheme
rests on. Read from `kernel/paging.c`, `kernel/paging.h`, `kernel/scheduler.c`,
and `boot/boot.asm`. For the rationale and trade-offs see
[decision 0012](../decisions/0012-per-process-paging.md).

## The problem it solves

Before this, everything shared the single boot tree (`boot/boot.asm`): one PML4,
one PDPT, one PD, all 2MB huge pages, identity mapped. Identity mapping means
virtual address equals physical address for everyone, so two tasks could not use
the same virtual address for different memory. That made tasks really threads:
their stacks had to sit at different addresses (a bump allocator split one shared
region, see [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md)), and
any task could read any other's memory with nothing to fault on.

Per-process paging gives each task its OWN tree. Both of the three programs run
their code at `0x400000` and put their stack top at `0x800000`, but land on
DIFFERENT physical frames. That difference is the isolation.

## Two halves: private user, shared kernel

Every tree is split the way every real kernel splits an address space.

**User half, PRIVATE per task.** 4KB pages to freshly allocated frames, user bit
set. This is what makes `0x400000` mean different physical memory in different
trees.

**Kernel half, SHARED (identical) in every tree, kernel-only.** The kernel must
be mapped in every tree, because when an interrupt fires the CPU jumps into
kernel code WITHOUT changing CR3: the first kernel instruction is fetched through
whatever task tree is currently loaded. If the kernel were not mapped there, the
CR3 switch itself would triple-fault on the very next instruction. So the IDT,
the GDT, the TSS `rsp0` stack, the interrupt stub, the kernel stack the interrupt
frame sits on, and all kernel code and data live in the kernel half of every
tree.

## Why the user half MUST be 4KB pages

A 2MB huge-page identity mapping forces virtual==physical: the page IS its own
physical address. That cannot give two tasks different physical memory at the
same virtual address. So the private user region must use 4KB pages, walked all
the way down to a PT whose leaf points at an arbitrary frame. The kernel region
stays 2MB huge pages (it is shared and identity mapped, so huge pages are fine
and cheaper). Each tree is therefore deliberately MIXED page sizes: 4KB user,
2MB kernel.

## Building a tree: clone the kernel half by value

`paging_create_address_space()` (`kernel/paging.c`) builds one tree:

1. Allocate an `address_space_t` handle on the kernel heap (kernel-only
   bookkeeping, safe there) and its own PML4, PDPT, and PD from `alloc_frame`
   (page tables must be 4KB and 4KB-aligned, which `alloc_frame` guarantees and
   `kmalloc` does not).
2. Fill the PD by COPYING every boot `pd_table` entry EXCEPT the two user slots
   (`pd_table[2]` = user code, `pd_table[3]` = user stack), which are left absent
   so the user branch can be overridden with private 4KB page tables. Each copied
   kernel entry carries the identical 2MB huge-page mapping (same physical kernel
   frame, no user bit), so every tree maps the identical kernel, kernel-only.
3. Wire `PDPT[0] -> this PD` and `PML4[0] -> this PDPT`, both user-permissive.

`paging_map_page(as, virt, phys, flags)` then installs the private user mappings,
walking `PML4 -> PDPT -> PD -> PT` and creating intermediate tables on first
touch. `paging_switch(as)` loads `as->pml4_phys` into CR3.

### By value, not by reference: why

The obvious approach is to share the kernel half BY REFERENCE (point every tree's
`PML4[0]` at the one boot `pdpt_table`/`pd_table`). It does not fit MiniOS's
layout. Everything hangs off a single branch, `PML4[0] -> PDPT[0] -> pd_table`,
and the ring-3 region lives INSIDE that same `pd_table` (user code is
`pd_table[2]`, user stack `pd_table[3]`). Sharing `pd_table` by reference would
share the user huge pages too, making a private 4KB mapping at `0x400000`
impossible: the whole point. So the kernel half is cloned by value, entry by
entry, skipping the two user slots.

## The load-bearing invariant: kernel mappings are frozen after boot

By-value cloning is correct ONLY because the kernel's mappings never change after
boot. `memory_detect_and_map` (`kernel/memory.c`) fills the identity map once at
startup, BEFORE any task tree exists, and nothing ever mutates a kernel PD entry
afterward. So a by-value copy taken at `task_create` time can never drift out of
sync with the boot tables.

This is a tripwire, not a footnote:

> If the kernel ever remaps itself at runtime (kernel ASLR, memory hot-plug, a
> higher-half relocation), the by-value copies already living in every existing
> task tree would silently go stale. The kernel would then see different mappings
> depending on which task happened to be current. At that point the by-value
> clone MUST become a by-reference share of the kernel tables, or gain an
> explicit step that propagates the change into every existing tree.

The same warning is at the `extern pd_table` clone site in `kernel/paging.c` and
in [decision 0012](../decisions/0012-per-process-paging.md). Do not add runtime
kernel remapping without revisiting it.

## The user bit at every level (AND-down)

x86 permits a ring-3 access only if the user bit (US, `PG_USER`) is set at EVERY
level of the walk: PML4, PDPT, PD, and PT. `paging_map_page` makes the
intermediate tables it creates user-permissive and lets the LEAF PTE gate the
real privilege. This is safe because every table it creates is on the user
branch. The cloned kernel leaves have NO user bit, so even though the branch
above them (`PML4[0]`/`PDPT[0]`) says "user may pass", ring 3 still cannot reach
kernel memory: the AND of the levels is kernel-only at the leaf.

## What a task's user half holds

`task_create` (`kernel/scheduler.c`) fills the private user half via
`build_user_space`:

- **Code.** The whole linked ring-3 image (`_user_text_start` to
  `_user_rodata_end`, all three programs) is COPIED into fresh frames mapped at
  its link address `0x400000`. The source is readable because the boot tables
  (still active at `task_create` time) identity-map `0x400000` to the image the
  bootloader loaded there. Copying the full image per task is the strongest
  isolation but duplicates the read-only text three times; `TODO(shared-text)`
  marks the by-reference-text refinement.
- **Stack.** Fresh frames mapped at a FIXED virtual address, top `0x800000`
  (`USER_STACK_TOP`) growing down `USER_STACK_SIZE` bytes. Every task reuses the
  same VA on its own frames, which is exactly what per-process paging makes
  possible and what retires the old shared-stack-region ceiling.

Because the user code stays at `0x400000`, nothing is relinked: each task's
forged `rip` is its own function's linked address, still valid in its private
copy.

## Switching CR3: where, and why it is safe

The switch happens in two places (`kernel/scheduler.c`):

- **`schedule()`** loads the incoming task's CR3 (`mov %cr3`, using the cached
  `task_t.cr3`) AFTER copying its register pile over the live interrupt frame,
  and before the stub's `iretq`.
- **`scheduler_start()`** loads task 0's CR3 (`paging_switch`) BEFORE the first
  drop to ring 3, since until then the boot tables are active and task 0's code
  and stack live in ITS tree.

This is safe mid-interrupt only because everything the CPU still needs on the way
out lives in the kernel half, cloned identically into every tree:

- the register pile `r` is on the KERNEL stack,
- the `tasks[]` array and scheduler code are kernel `.data`/`.text`,
- when the timer next fires, the IDT, GDT, TSS `rsp0` stack, and interrupt stub
  are all reached through the same kernel mappings.

So the switch changes only the user half; the kernel never disappears out from
under itself. If any of those lived in the user half, the switch would
triple-fault on the next instruction. The ordering trap: the CR3 write MUST come
after the scheduler is done reading its own state and before `iretq` returns to
ring 3.

## CR3 holds a physical address, and the write flushes the TLB

CR3 takes a PHYSICAL address. Everything below 1GB is identity mapped, so
`as->pml4_phys` doubles as both the physical base of the PML4 and a writable
virtual pointer to it. Writing CR3 also flushes the TLB, because MiniOS marks no
page `PG_GLOBAL`, so no entries survive the write. That is exactly what drops the
outgoing task's stale user translations on a switch, for free, with no explicit
`invlpg`.

## What a run looks like

Booted under QEMU with `-d int`, the three programs interleave "A", "B", "C"
forever, and the log shows:

- three distinct task CR3 values (one tree per task), after a single tick on the
  boot CR3 before the first switch,
- the same three user RIPs as before (`0x40002b`, `0x400083`, `0x4000db`), all at
  `cpl=3`, now each in its own tree,
- an even round-robin interleave,
- zero page faults (`0x0E`), GP faults (`0x0D`), or double faults (`0x08`).

A temporary isolation proof (added, verified, then removed) walked each task's
tree and confirmed the shared VAs `0x400000` and the stack top page resolve to
DIFFERENT physical frames in all three trees. Same virtual address, different
physical memory: the isolation, demonstrated.

Failure modes to recognise: a triple fault (`0x08`) right after the first switch
means something the kernel needs is not in the cloned kernel half. A page fault
at a user RIP means the private user mapping for that task is missing or wrong.

## Related

- The decision and its trade-offs:
  [decision 0012](../decisions/0012-per-process-paging.md).
- The scheduler that loads CR3 on switch:
  [scheduling.md](scheduling.md).
- The physical layout and the fixed user virtual addresses:
  [memory-map.md](memory-map.md).
- The shared-region layout this replaces:
  [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md).
- Concepts behind virtual memory and page tables:
  [`../../learnings/05-memory-management.md`](../../learnings/05-memory-management.md).
</content>
