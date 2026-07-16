# 5. Memory Management

**Source files:** `kernel/memory.c`, `kernel/memory.h`, `libc/mem.c`

Memory management is where operating systems get genuinely deep. This chapter
builds the mental model from the ground up, shows what MiniOS implements (a
physical allocator), and clearly marks where MiniOS *stops* and real kernels keep
going (virtual memory and paging). Knowing the boundary is as valuable as knowing
the code.

## The big idea

Two distinct problems hide under the phrase "memory management," and conflating
them is the #1 source of confusion:

1. **Allocation** — "I have a big pool of RAM; who is using which bytes?" This is
   bookkeeping: hand out chunks, track them, take them back. `malloc`/`free` live
   here.
2. **Address translation & protection** — "Each program thinks it owns all of
   memory, but they must not actually collide or spy on each other." This is
   **virtual memory**, implemented with **paging**, enforced by the hardware MMU.

MiniOS implements a piece of #1 and leaves #2 as the clearly-marked next frontier.
Real kernels do both, deeply intertwined.

## Physical vs. virtual memory

**Physical memory** is the actual RAM chips, addressed `0` up to however many
bytes you have. **Virtual memory** is a per-program illusion: every process gets
its own private, contiguous address space (say, a full 4 GB on 32-bit), and the
hardware transparently translates each virtual address to some physical address —
or faults if the program touches something it shouldn't.

Why bother with the illusion? Virtual memory buys three enormous things:

- **Isolation.** Process A's address `0x1000` and process B's address `0x1000` map
  to *different* physical bytes. A cannot see or corrupt B. This is job #3 from
  chapter 0.
- **Simplicity for programs.** Every program is compiled as if it owns memory
  starting at a fixed address. The OS relocates it invisibly. No program has to
  know where in physical RAM it actually landed.
- **Overcommit and paging to disk.** The "memory" a program sees can exceed
  physical RAM; unused pages get written to disk and faulted back in on demand.
  This is why you can open more tabs than you have RAM.

MiniOS runs entirely in **physical memory** — every address the code uses is a
real RAM address. That is fine for a single-privilege-level kernel with no user
programs, and it keeps the whole system comprehensible.

## What MiniOS builds: a physical memory allocator

Per the build spec, `memory.c` is a **physical memory bitmap allocator**. The idea
is beautifully simple:

- Divide physical RAM into fixed-size **frames** (typically 4 KB — the page size,
  so this allocator will play nicely with paging later).
- Keep a **bitmap**: one bit per frame. `1` = used, `0` = free.
- To allocate a frame, scan the bitmap for a `0` bit, set it to `1`, and return
  that frame's address.
- To free a frame, clear its bit back to `0`.

A bitmap is memory-efficient (one bit tracks 4 KB = 32,768 bits of RAM) and
trivially correct. Its weakness is allocation speed — finding a free frame is a
linear scan — which is why production allocators use fancier structures (below).
For learning, the bitmap is perfect: you can hold the entire mechanism in your
head.

### Frames vs. bytes

Notice the granularity choice. This allocator hands out **whole 4 KB frames**, not
arbitrary byte counts. That is deliberate: physical frame allocation is the
*bottom* layer. A byte-granular `malloc` (a "heap allocator" like `kmalloc`) would
be built *on top*, carving small objects out of frames it requested from this
allocator. Real kernels have this two-level structure: a **page/frame allocator**
underneath, a **heap allocator** on top. MiniOS gives you the bottom layer.

## The libc memory helpers (`libc/mem.c`)

Separate from allocation are the humble byte-movers `memcpy` and `memset`. These
are not "memory management" in the OS sense — they just copy and fill bytes — but
a freestanding kernel has no standard library, so it must supply its own
(chapter 7). You have already seen them used: `screen.c`'s `scroll()` calls
`memcpy`, and zeroing the IDT is conceptually a `memset`. They are byte-by-byte
loops; correctness over cleverness.

## Where MiniOS stops: paging (the next frontier)

MiniOS does **not** set up paging. Here is what you would add to cross into real
virtual memory, so you know the shape of the gap:

### Page tables

Paging translates virtual → physical in fixed-size **pages** (4 KB on x86). The
translation is described by **page tables** the OS builds in memory. On 32-bit x86
it is a two-level tree:

```
virtual address (32 bits)
  = [ 10 bits: page directory index ][ 10 bits: page table index ][ 12 bits: offset ]

CR3 register → page directory → page table → physical frame → + offset
```

The `CR3` register points at the top-level directory — the same "special register
points at a table" pattern as the GDT (`lgdt`) and IDT (`lidt`). Setting bit 31 of
`CR0` turns paging on. From that instant, *every* memory access is translated by
the hardware **MMU**, with recently-used translations cached in the **TLB**.

### The page fault: where paging meets chapter 3

When a program touches a virtual address that is not mapped (or violates
permissions), the CPU raises **interrupt 14, the page fault** — one of those
exceptions that pushes an error code (chapter 3). The error code says whether it
was a read/write, and whether the page was present. The page-fault handler is one
of the most important functions in a real kernel: it implements demand paging,
copy-on-write (how `fork()` is fast), stack growth, and memory-mapped files. All
of it hangs off that one interrupt handler you already have the machinery for.

This is the payoff of learning interrupts first: virtual memory is *built on* the
interrupt system. Chapter 3 was the foundation; paging is one of the tallest
buildings you can put on it.

## Going further

- **Heap allocators**: once you have a frame allocator, build `kmalloc`/`kfree` on
  top for arbitrary-size kernel objects. Classic designs: free lists, buddy
  allocators (Linux's page allocator), and slab allocators (for many same-size
  objects like `task_struct`s).
- **The Linux picture**: a **buddy allocator** manages physical frames, **slab/
  slub** manages kernel objects, per-process **page tables** provide isolation, and
  the **page-fault handler** implements demand paging and copy-on-write. Every one
  of those is a scaled-up, hardened version of an idea in this chapter.
- **`mmap` and the unified model**: modern kernels treat files and memory
  uniformly — mapping a file into a process's address space so that reading memory
  reads the file, paged in on demand via faults.

### Exercises

1. A bitmap uses 1 bit per 4 KB frame. How many bytes of bitmap do you need to
   track 128 MB of RAM? How about 4 GB?
2. Why choose a 4 KB frame size specifically? What ties the frame allocator's
   granularity to the eventual paging system?
3. Sketch the page-fault handler you would register on interrupt 14. What are the
   two questions it must answer before deciding to map a page vs. kill the program?
4. MiniOS uses physical addresses directly. List two concrete things that become
   *possible* only once you add paging (revisit the "three enormous things" above).
