# Chapter 14: Per-Process Paging

> Read chapter 8 (long mode and paging) and chapter 10 (user mode) first. Chapter 8
> built the map. Chapter 10 used it to keep programs out of the kernel. This chapter
> is the other half of that job.

## Where we are

There is one map, and everybody shares it.

That was enough to keep ring-3 programs out of the kernel: the leaves covering
kernel memory have the user bit clear, so a program that reaches for them faults.
Chapter 10 did its job.

But look at what it did not do. Every ring-3 program is loaded at 0x400000, because
that is where the user region begins and there is only one of it. Two programs are
not merely near each other, they are *the same memory*. Start a second one and it
loads directly on top of the first.

Programs cannot break the kernel. They can absolutely break each other, and right
now they do it by existing.

## Rule 1: one map is a shared house

Useful way to hold it. There is one building. The kernel's rooms are locked and no
program can get into them. Every program is in the remaining rooms, together, with
no walls between them.

Isolation from the kernel is half the job. This chapter is the other half.

## Rule 2: give each program its own map

That is the feature. The whole feature.

Each task gets its own page table tree. The scheduler, which already switches
register piles when it switches tasks, also loads that task's tree into CR3.

And now something strange and excellent is true: **address 0x400000 is a different
physical location depending on which map is loaded.** Same number. Different memory.
Two programs can both use it, both be correct, and never touch each other.

Chapter 8 said an address is a question rather than a location. This is where that
pays. The answer now depends on who is asking.

## Rule 3: what "address space" actually means

The phrase can be defined properly now, and it is worth doing, because it gets used
loosely everywhere.

**An address space is a map, plus everything reachable through it.**

Two tasks with different maps are in different address spaces, and here is the part
that matters: there is no way for one to *name* memory in the other. Not "it is
forbidden." Not "the kernel checks." The number simply does not denote that memory.

A program asking for 0x400000 gets its own 0x400000, in the same way that "the
third house on my street" does not refer to anything on your street. It is not that
you are refusing to let me point at your house. It is that my sentence does not
point there.

That is why the metaphor in these chapters is **streets**. Each program lives on its
own, house numbers repeat between them, and a house number on its own is not an
address at all until you say which street.

## Rule 4: failures become local

The obvious payoff is security. The everyday payoff is debugging, and it is bigger
than it sounds.

Before: task A writes through a bad pointer into task B's data. B does not notice.
B fails twenty seconds later doing something unrelated, and every piece of evidence
points at B, which is not where the bug is. You can lose a day to that, and the
day does not feel like it is being lost, because you are busy the whole time.

After: A's bad pointer either lands in A's own memory, which is A's bug producing
A's crash, or it lands outside A's map and faults immediately, at the instruction
that did it.

**Isolation is not primarily about malice. It is about blame.** It makes failures
stay where they were caused.

## Rule 5: the kernel has to be in every map

Here is the constraint that shapes everything, and it is not obvious until you see
why it is absolute.

An interrupt can arrive at any moment. When one does, the CPU immediately starts
executing kernel code. And CR3 has not changed: the handler runs on whatever map the
interrupted task had loaded.

So if the kernel is not present in that task's map, the very first instruction of
the handler cannot be fetched. That is a page fault. The page fault handler is also
kernel code, also not mapped, so that fault cannot be handled either. That is a
double fault, which also cannot be handled, and the CPU gives up and resets.

**The kernel must be reachable through every task's map, or no task can ever be
interrupted.** And a task that cannot be interrupted cannot be preempted, which
means everything in chapter 12 stops working too.

There is no way around this. Every operating system that has ever used paging has
the kernel mapped into every address space, for exactly this reason.

## Rule 6: present, and forbidden

So every task's tree contains the same kernel entries. TownOS copies them in when it
builds a new tree: identical entries, pointing at identical physical memory, in
every map in the machine.

And the user bit on those entries is clear.

That combination is the answer, and it is worth saying slowly because the two halves
feel contradictory:

- **Mapped**, so the kernel can execute when an interrupt lands, whichever task was
  running.
- **Forbidden**, so the program whose map it is cannot read a byte of it.

**The kernel is in every house, behind a locked door.** It has to be in the house,
because the fire brigade has to be able to get in from anywhere. The lock is what
stops the residents wandering in.

## Rule 7: the copy is of the entry, not the memory

A distinction that causes real confusion, and it matters again in chapter 20.

When a new tree is built, the kernel's page directory entries are copied **by
value**. So each task has its own copies of those entries.

But an entry is a pointer. The physical memory those entries describe is not copied
and is not per-task. There is one kernel in memory, and forty tasks each holding
their own copy of the entry that points at it.

Which means when a task dies and its map is torn down, freeing the memory the kernel
entries point at would free the kernel out from under the entire machine. The
teardown has to free only the entries that are genuinely the task's own.

**The entry is a copy. The thing it points at is shared.** Get that backwards during
cleanup and the machine dies with no message at all.

## Rule 8: what isolation costs

Two costs, both real.

**The TLB.** The CPU caches recent address translations, because otherwise every
memory access would mean four extra lookups. Loading CR3 invalidates that cache,
because the translations in it belong to a map that is no longer loaded. So every
task switch is followed by a stretch of slow memory accesses while the cache refills.

Modern CPUs can tag cached translations with an address-space identifier so that
entries from different maps can coexist and survive a switch. TownOS does not use
that. Every switch throws everything away.

**Memory for the tables.** Each task needs its own tree.

Which turns out to be nearly free, and that is chapter 8's tree paying off again.
Three frames for the top three levels, plus a table for each 2MB region of user
memory actually in use. Because branches that describe nothing do not exist, giving a
task a private 64-bit address space costs about twelve kilobytes.

A flat map would have made this feature impossible. The tree makes it routine.

## What this still is not

- **No shared memory.** There is no way for two tasks to deliberately share a page.
  Complete isolation is easier than selective isolation, and selective is what you
  need for pipes and shared libraries.
- **No copy-on-write, no `fork`.** Real systems create a process by cloning an
  existing one and sharing every page read-only until somebody writes. That trick is
  entirely a paging trick and it needs a fault handler that can do work rather than
  just report.
- **No demand paging.** Everything in a map is backed by real memory right now.
  Nothing is loaded lazily on first touch.
- **Two tasks running the same program hold two complete copies of its code.** The
  code is identical and read-only and could obviously be shared. Nothing shares it.

## Exercises

1. Two tasks both use address 0x400000. Explain, in terms of the page walk, why they
   do not collide.
2. Suppose the kernel were mapped into only the currently running task's map, and
   removed on every switch. Trace what happens on the next timer tick.
3. The kernel's entries are present in every map with the user bit clear. Explain
   what each of those two properties is buying, and what breaks if you drop either.
4. A task's map is torn down when it exits. Explain precisely which frames must be
   freed and which must not, and what the symptom is if you free one too many.
5. Loading CR3 discards the TLB. Estimate what that costs at a 10 millisecond
   quantum, and say whether you would expect it to be noticeable.
6. A private 64-bit address space costs about 12KB of tables. Explain why, and say
   what the figure would be with a flat page table.
7. Before this chapter, a bad pointer in one program could corrupt another. Describe
   a debugging session for that bug, and then describe the same bug after this
   chapter.
8. Two tasks run the same program and hold two copies of its code. Sketch what would
   be needed to share it, and name the thing that makes it harder than it sounds.
