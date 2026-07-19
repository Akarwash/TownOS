# 0010 - Port the p5 explicit-free-list allocator as the kernel heap

## Status

Accepted.

## Context

The kernel could allocate physical memory only one way: `alloc_frame()`
(`kernel/memory.c`) hands out whole 4KB frames. Anything smaller wasted the rest
of the frame, and there was no way to allocate an arbitrary-size object at all.
The scheduler shows the cost directly: its task table is a fixed `.bss` array of
four (`MAX_TASKS`), with a standing TODO to make it dynamic, precisely because
there was no heap to allocate a ~200-byte `task_t` from. Per-process page tables,
variable-length buffers, and any growable kernel data structure were blocked on
the same missing layer.

A complete, working explicit-free-list allocator already existed: the CMSC216 p5
`el_malloc`. It has header/footer boundary tags, coalescing with the neighbours
above and below, two doubly-linked lists (available and used) with dummy
begin/end nodes, first-fit search, and block splitting. Its only OS dependency is
that it gets its slab from `mmap` and grows it with more `mmap`. Reimplementing an
allocator from scratch would risk subtle boundary-tag bugs the p5 code has
already worked out, so the decision was to port the real code, not invent a new
one.

Three facts about the kernel shaped the port. There is no `mmap`, only the frame
allocator. There is no libc, so `printf`/`assert`/`<stdint.h>` are unavailable.
And unlike the single-threaded p5 test harness, the 100 Hz timer interrupt can
land in the middle of a list relink and the handler may itself allocate.

## Decision

Port `el_malloc` into `kernel/heap.c` / `kernel/heap.h` essentially verbatim,
changing only the three OS seams, and expose it as `kmalloc`/`kfree`.

- **Slab source: `mmap` becomes the frame allocator.** Add
  `alloc_frames_contiguous(n)` to `kernel/memory.c`, next to the frame allocator
  it belongs with. The bitmap is linear and the whole pool is identity-mapped, so
  a run of `n` consecutive clear bits is `n` contiguous, mapped physical frames.
  `el_init` and the growth path both draw their slab from it, which keeps the p5
  code's single-contiguous-slab assumption valid. This is **contiguity approach
  (b)** from the plan (one real contiguous slab), chosen over (a) per-frame arenas
  because the linear bitmap makes contiguity cheap and (b) leaves the ported code
  unchanged. The fixed target addresses (`EL_HEAP_START_ADDRESS`) and the
  `mmap`-returned-what-I-asked asserts are dropped; the heap lives at whatever
  base the allocator returns. `el_ctl` becomes a static `.bss` struct rather than
  an `mmap`'d page.
- **No libc.** `printf`/`fprintf` become the VGA `print_string`, with small
  kernel-native decimal and hex printers for the stats helpers. `assert` and the
  `mmap`-return checks are dropped (the conditions they guarded no longer exist).
  Types come from `include/types.h`.
- **Interrupt safety.** `kmalloc`/`kfree` wrap their critical section in a
  save-and-restore guard: read RFLAGS via `pushfq`, `cli`, do the work, restore
  the saved flags. It is deliberately not an unconditional `sti`: `kmalloc` may be
  called from inside an interrupt handler, where interrupts must stay off. The
  guard lives in the `kmalloc`/`kfree` wrappers so the ported `el_malloc`/`el_free`
  core stays the pure p5 algorithm.

The public interface is `kmalloc`/`kfree`; the internal `el_*` names are kept
(they match the original comments). `heap_init()` runs from `kernel_main` after
`memory_init()`, builds a 16-page (64KB) initial slab, and the heap grows on
demand: when `el_malloc` finds no block large enough, the `kmalloc` wrapper calls
the growth path for enough pages and retries.

## Consequences

- Arbitrary-size kernel allocation now exists. This unblocks the natural next
  steps: a dynamically sized task table (retiring the fixed `MAX_TASKS` of four)
  and per-process page-table structures. Those are not built here; only the
  allocator they need is.
- First-fit has fragmentation the design accepts. It is the p5 algorithm
  unchanged; a learning kernel does not need best-fit or a slab/buddy allocator
  yet.
- The interrupt guard serialises all allocation: while one `kmalloc` runs,
  interrupts are off, so the timer cannot preempt it. At 100 Hz with short
  critical sections this is fine, but it is a global lock in spirit and would need
  revisiting under SMP (which MiniOS does not have).
- **Contiguity and its limits.** Growth requires the new run to sit immediately
  above `heap_end`. Because the heap is the sole consumer of the frame allocator
  after `memory_init` and the bitmap is scanned bottom-up, growth is adjacent by
  construction, so the single contiguous slab stays intact. A non-adjacent run is
  refused and its frames reclaimed rather than spliced in as a disjoint region:
  the boundary-tag walk (`el_block_above`/`el_block_below`) is bounded by a single
  `heap_start`/`heap_end` pair and would step into the gap between two regions,
  reading garbage. Refusing keeps the invariant that the whole heap is one
  contiguous, walkable slab. If a future change makes the heap share the frame
  allocator with other consumers, growth could become non-adjacent and this
  branch would (correctly) fail loudly rather than corrupt the heap.
- The heap is still one shared address space with no isolation, exactly like the
  frame allocator beneath it. It allocates; it does not protect. See
  [0009](0009-read-multiboot-map-extend-identity-map.md) and
  [../reference/heap.md](../reference/heap.md).
- Verified under QEMU before the temporary self-test was removed: allocation with
  sentinel readback, free-and-reuse returning the same block, full coalesce back
  to a single free block, and the on-demand growth path all passed, with zero page
  faults or GP faults and the timer and syscall vectors still firing (the two
  ring-3 tasks print "ABAB" unchanged).
