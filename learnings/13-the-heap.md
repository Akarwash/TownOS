# Chapter 13: The Heap

> Read the chapter on the frame allocator first. This one is a layer directly on
> top of it, and the whole chapter is about the gap between what that layer hands
> out and what the kernel actually wants.

## Where we are

The frame allocator hands out 4KB frames. That is the unit the hardware cares about,
because it is the unit the page tables describe.

It is the wrong unit for almost everything the kernel wants. A task struct is a few
hundred bytes. A filename buffer is twelve. An address space handle is sixteen. Ask
the frame allocator for one of those and you get 4096 bytes and waste 99% of it.

So you need a layer that carves frames into arbitrary sizes.

Keep the two straight, because the words blur: **frames are pages, `kmalloc` is
bytes on pages.**

## Rule 1: allocating is easy, freeing is the problem

This is the sentence that explains why allocators are a whole subject.

Handing out memory forwards is trivial. Keep a pointer to the next unused byte,
return it, move it up by the requested size. That is a bump allocator, it is about
five lines, and if nothing is ever freed it is genuinely all you need. Plenty of
real programs use exactly that.

Every difficulty in this chapter comes from `free`.

Look at what freeing asks that allocating does not:

- **How big was this block?** The caller passes back a pointer and nothing else. It
  does not tell you the size. It probably does not remember.
- **Where does the space go, so it can be found again?**
- **Can it be joined to its neighbours?** If the block before and after are also
  free, three small holes should become one big one.

Allocating answers none of those questions. It only has to move a pointer.

## Rule 2: so the allocator remembers, inside the memory itself

The answer to "how big was this block" has to be stored somewhere, and the natural
place is right next to the block.

So every block carries a **header**, a few bytes immediately before the pointer you
were handed, holding its size and whether it is in use.

That is why `kmalloc(100)` consumes more than 100 bytes of heap. And it is why
`kfree` needs only a pointer: step backwards from it by the size of the header and
everything the allocator needs is right there.

**Every allocator you have ever used is doing this.** The bytes you did not ask for
are how `free` knows what to do when you hand the pointer back. If you have ever
wondered how `free(p)` can possibly know how much to release, that is the answer, and
it has been sitting a few bytes behind your pointer the whole time.

## Rule 3: fragmentation, and there are two kinds

**Internal fragmentation** is waste inside a block. You asked for 100 bytes, the
allocator rounded up and gave you a block of 128, and the 28 bytes are yours and
useless. Bounded, predictable, and mostly harmless.

**External fragmentation** is the nasty one. There are 4KB free in the heap and a
1KB request fails, because the free space is in forty scattered pieces and none of
them is big enough.

That failure is genuinely maddening the first time you meet it, because the allocator
appears to be lying. There is plenty of memory. It is just in the wrong shape.

External fragmentation is the reason the next rule exists.

## Rule 4: coalescing, and the trick that makes it possible

When a block is freed, you want to merge it with any free neighbours, so that a run
of frees collapses back into one large free block instead of leaving a row of small
useless ones.

Merging with the block **above** is easy. You know your own size, so add it to your
address and you land exactly on the next block's header. Read its state, and if it
is free, absorb it.

Merging with the block **below** is where it gets interesting.

You are standing on your own header. Where is the previous block's header? You have
no idea. Blocks are different sizes, so you cannot subtract a constant. The only way
to find it is to walk the entire heap from the beginning, checking each block's size
until you find the one that ends where you begin. That is O(heap) on every single
free.

The fix is a **boundary tag**: put a copy of the size at the *end* of every block as
well as the start. The footer.

Now finding the block below is: step back a few bytes from your own header, and you
are standing on the previous block's footer. Read the size it stored. Jump back that
far. You are on its header. Constant time.

**Every block in the heap pays for a footer so that freeing can look backwards.**

That is the trade, and it is worth stating in exactly those terms, because it is the
shape of a great many systems decisions: pay a small fixed cost on every object, in
order to make one important operation cheap instead of catastrophic.

## Rule 5: split and coalesce are mirror images

Two operations reshape the heap and they are exact inverses.

**Split.** A request comes in for 100 bytes and the first free block is 4000. Rather
than hand over 4000, cut a header and footer pair into the middle: the low part
becomes a 100-byte block that gets returned, the high part becomes a new smaller free
block. One block became two, and one header/footer pair was added.

**Coalesce.** A block is freed and its neighbour is free too. The divider between
them is removed, and the merged block reclaims the header and footer that used to
separate them. Two blocks became one, and one header/footer pair went away.

Split adds a divider. Coalesce removes one. The heap breathes in and out and the
total overhead tracks the number of live blocks, which is exactly right.

## Rule 6: finding a free block fast

You cannot scan every block looking for a free one, because most blocks are in use
and you would walk past all of them.

So free blocks are threaded onto a linked list, and the list pointers live **inside
the free blocks themselves**.

That costs nothing, and it is a lovely idea. A free block is by definition space
nobody is using. Storing bookkeeping in it is free. The moment it gets allocated, the
pointers are meaningless and get overwritten by the caller's data, which is fine,
because it is no longer on the list.

**A free block stores the bookkeeping in the space it is not using.**

The search policy here is **first fit**: walk the list and take the first block big
enough. The obvious alternative is best fit, which scans the whole list for the
closest match. Best fit sounds better and mostly is not: it is slower, and it
deliberately produces the smallest possible leftover fragment every time, which is a
great way to fill your heap with slivers too small to use.

## Rule 7: dummy nodes, or how to delete an edge case

A small technique worth stealing.

Both lists have dummy nodes at the beginning and the end that hold no block. They
exist so that there is always a node before and after any real node.

Which means insertion and removal never have to ask "am I at the start" or "am I at
the end." The pointer updates are the same four lines in every case. Ten lines of
special-casing become four lines with no branches.

**Add a fake element so the edge case stops existing.** You will use this again.

## Rule 8: the seam with the layer below

The original of this allocator got its memory from `mmap`, because it was a userspace
program on a real operating system. There is no `mmap` here, because you are the
operating system.

So the seam is a request to the frame allocator for a **contiguous** run of frames,
16 of them, 64KB, and the first block is built inside that.

The word "contiguous" is doing real work. This allocator assumes its blocks live in
one continuous span of addresses, because "the block above" is computed by adding a
size. Hand it a set of frames that were not adjacent in physical memory and every
upward walk lands in the middle of something unrelated.

That is the kind of assumption worth writing down where somebody will find it, since
it is invisible from inside the allocator and fatal from outside.

## Rule 9: it was ported, not written

Worth its own section because it was a real engineering decision, and a good one.

This allocator came from a CMSC216 assignment. It already existed, already worked,
and had already been debugged against a test suite that tried hard to break it.

Writing a new one would have meant debugging a fresh allocator **inside a kernel**,
where there is no debugger, no `printf` that survives a crash, and where an allocator
bug does not announce itself. It corrupts a header, and something unrelated dies
thirty seconds later with no evidence pointing anywhere near the allocator.

Porting it meant changing where it gets its memory from and nothing else.

The lesson generalises further than allocators: **the most valuable property of a
piece of code is that it has already been wrong and been fixed.** New code has not
been wrong yet, which is not the same as being right.

## What this still is not

- **This is the kernel's heap.** Ring-3 programs have no `malloc`. They get a stack
  and their loaded image, and that is all.
- **No `realloc`, no `calloc`.** Just allocate and free.
- **It never shrinks.** Frames handed to the heap are never given back to the frame
  allocator, even if the entire heap becomes free.
- **No double-free detection, no guard pages, no poisoning.** Free a pointer twice
  and the free lists corrupt quietly. Write one byte past the end of your block and
  you have overwritten the next block's header, and the damage surfaces on some
  later, unrelated allocation.
- **It is not thread-safe, and it does not need to be.** There is one kernel and only
  one task is ever inside it at a time, because system calls run with interrupts
  masked (chapter 11). That is a real dependency, not a coincidence, and it is worth
  knowing which other design decision is holding this one up.

## Exercises

1. `free(p)` takes only a pointer. Explain how it knows the size, and say exactly
   where that information lives relative to `p`.
2. Give an example of internal fragmentation and one of external fragmentation, with
   numbers. Which would you rather have, and why?
3. Merging with the block above needs no footer. Merging with the block below is
   impossible without one. Explain the asymmetry in terms of what you know when you
   are standing on a header.
4. Every block pays for a footer. For a heap of 1000 blocks averaging 64 bytes each,
   what fraction of the heap is overhead? Does that change your opinion of the
   design?
5. Split and coalesce are inverses. Describe a sequence of allocations and frees that
   returns the heap to exactly its starting state, and one that does not.
6. Free-list pointers are stored inside free blocks. What happens to those bytes when
   the block is allocated, and why is that safe?
7. Best fit finds a closer match than first fit. Explain why it is usually the worse
   choice anyway.
8. The heap requires its frames to be contiguous. Describe the failure if it were
   given a non-contiguous run, and say how long it would take you to work out what
   had happened.
9. The allocator is not thread-safe and does not need to be. Name the specific
   property of the syscall path that makes that true, and say what would have to
   change first.
