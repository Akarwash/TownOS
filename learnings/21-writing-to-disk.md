# Chapter 21: Writing to Disk

> Read chapter 16 (the filesystem) first. This chapter assumes clusters, the FAT,
> directory entries and the three questions. It is the other half of that chapter,
> and chapter 16 ended by telling you this one would be harder.

## Where we are

You have a front desk. It can answer all three questions (which lockers are free,
which lockers hold this file, what is it called) by reading what the format left
on the disk.

One thing is already in place that is easy to miss. `disk_write` in
`drivers/disk.c` exists and works. It has worked since chapter 15. The lockers have
always accepted new contents; nobody at the front desk has ever asked them to.

So everything missing for this chapter is at the desk, not below it.

## Rule 1: reading is a walk, writing is a change

Chapter 16 said writing is where filesystems get hard. Here is the specific reason,
and it is not the amount of code.

A read that goes wrong shows you garbage. You look at the garbage, you fix the bug,
you run it again. The disk is untouched the whole time.

A write that goes wrong changes the disk. And the disk is not yours alone. Your
kernel reads it every boot to find `SHELL.ELF`. `mtools` on your host machine reads
it to put programs on. A future version of your own filesystem code will read it.
A bad write can make the machine unbootable, and the machine stays unbootable after
you fix the bug, because the damage is not in the code any more. It is on the
platter.

That asymmetry is the whole character of this chapter. It is why the first thing
the build prompt tells you to do is copy `disk.img` somewhere safe.

## Rule 2: the three questions become three edits

Reading answers the three questions by looking. Writing answers them by changing.
And the three answers live in three different places on the disk.

| Question | Read | Write |
|---|---|---|
| Which lockers are free? | Look for a FAT entry that is 0 | Write something non-zero into it |
| Which lockers hold this file? | Follow the chain | Write each entry to point at the next |
| What is it called? | Read the directory entry | Write a 32-byte directory entry |

That is the whole feature. Three edits.

Every difficulty in this chapter comes from the fact that they are three separate
writes and not one. Hold onto that. It explains everything that follows.

## Rule 3: there is no allocator to build

This is the part that surprises people, and it is worth stating plainly because
the instinct is to go looking for something that is not there.

You do not need a free-space data structure. **The FAT already is one.** An entry of
0 means that cluster is free. Claiming cluster 47 means writing a non-zero value
into FAT[47]. There is no bitmap to maintain, no free list to thread, no allocator
to design. The exact table you already parse in order to follow a chain is the
table you allocate from.

Compare that to the frame allocator you wrote for physical memory, which needed its
own bitmap because RAM has no format on it. A disk does. FAT32's designers put the
free map and the chain map in the same structure, and that is genuinely the clever
bit of the format.

### What you do need is the search

Free space is not indexed. Finding a free cluster means walking the table looking
for a zero.

Your disk is 64MB with 512-byte clusters, so there are 131072 clusters and the FAT
is half a megabyte. Scanning it entry by entry, reading a 512-byte block each time
to look at four bytes, is 131072 disk reads for one allocation. That is not a
performance nitpick, that is unusable.

Two things fix it and you want both. Read the table a block at a time and scan the
entries in memory. And remember where you stopped, so the next allocation starts
there instead of at the beginning.

That second one is exactly what the FSInfo sector exists for. FAT32 has a whole
reserved sector whose job is to cache "here is roughly how much is free, and here
is where to start looking". It is a hint, not truth, and the format says so.

## Rule 4: the last write is the one that makes it true

This is the important rule in the chapter. Everything above is mechanics.

You are making three edits. Between any two of them the power can go out, or QEMU
can be killed, or your kernel can triple fault. So the order you make them in is
not a matter of taste. **The order decides what a half-finished write leaves
behind.**

Take the good order first: fill the data cluster, then link the FAT, then write the
directory entry.

**Dies after the data write.** You put bytes into a cluster that nothing points at.
The FAT still says it is free. The directory has never heard of it. The next
allocation hands that cluster to somebody else and overwrites your bytes. Nothing
is wrong. Nothing even needs cleaning up.

**Dies after the FAT link.** A cluster is marked in use, but no directory entry
names it. Space is consumed that nothing can reach. This has a name, a **lost
cluster**, and it is most of what a disk checker ever finds. It wastes space. It
corrupts nothing.

**Dies after the directory entry.** You are done. Everything agrees.

Now do it backwards. Write the directory entry first, then link the FAT, then fill
the data.

**Dies after the directory entry.** The directory says `notes.txt` is 4000 bytes
starting at cluster 47. FAT[47] says cluster 47 is free. Read the file and you get
whatever happened to be in cluster 47, which is somebody's deleted email or a
fragment of an old program. Worse, the next allocation looks at FAT[47], sees free,
and hands it out. Now two files own the same bytes and writing one silently
destroys the other.

Same three writes. One order costs you some wasted space in the worst case, the
other costs you data.

### So: allocate and fill first, link second, publish the name last

The directory entry is the **commit point**. Before it is written, nothing in the
world refers to your new file. After it is written, everything agrees about
everything. There is no moment in between where the disk claims something false.

That is not a FAT32 idea. That is the same discipline a database uses when it
writes a transaction and then flips a single commit flag at the end. FAT32 has no
journal (chapter 16 said so, under "what a filesystem still is not"), so ordering
is the only crash safety available. It is free, and it is the difference between
losing space and losing data.

### Replacing a file is the same rule, one level up

Say `NOTES.TXT` already exists and you write to it again.

The obvious move is to free the old chain first, then write the new one. Do not.
If anything goes wrong after the free and before the new entry is published, you
have destroyed a file that was perfectly good and put nothing in its place.

The right order falls straight out of rule 4. Allocate a **new** chain. Fill it.
Point the directory entry at it. **Then** free the old chain.

The old file stays completely intact and readable right up until the instant the
new one is complete, and then it stops existing. Same commit point, same guarantee,
one more step at the end.

### Deleting runs the other way

Delete is the mirror image, and so is its ordering.

Free the data first, then unpublish the name. Die in between and you have a lost
chain, which is wasted space. The other order gives you a live directory entry
pointing at clusters that are now marked free and will be handed to somebody else,
which is the corruption case again.

The rule generalises: **the thing that makes a claim about other things is written
last on the way in, and cleared first on the way out.**

## Rule 5: directories fill up too

Chapter 16 told you a directory is just a file whose contents happen to be 32-byte
entries. When you were reading, that was a curiosity. Now it bites.

Creating a file means finding a free entry slot. A slot is free if its first name
byte is 0x00 (never used) or 0xE5 (deleted). If every slot in every cluster of the
root directory's chain is taken, you cannot create the file until the directory
grows.

Growing it is the same operation you just wrote: allocate a cluster, link it onto
the end of the directory's chain. Same code, different caller. That is the payoff
of "a directory is a file" being literally true rather than a teaching
simplification.

One thing you must not forget: **zero-fill the new cluster.** A directory is read
32 bytes at a time and every window is interpreted as an entry. An un-zeroed
cluster is full of whatever used to be there, and all of it will be read as
directory entries with garbage names, garbage sizes, and garbage start clusters.

Your clusters are 512 bytes, so a cluster holds exactly 16 entries. That means you
can reach the growth case by creating seventeen files, which is a gift. On a real
volume with 32KB clusters you would need a thousand files to test the same path,
and you would not bother, and it would be broken.

## Rule 6: there are two FATs

Chapter 16 explained why FAT32 keeps two identical copies. It did not matter then,
because reading only ever consults the first.

It matters now. **Every FAT edit has to land in both copies.** Update one and they
disagree, and the volume is by definition corrupt. A repair tool that finds them
disagreeing has to guess which one is right, and it may guess wrong.

This is the single most likely bug in the whole rung, and it has a nasty property:
your own kernel will not notice. Your reader only ever looks at the first copy, so
everything works perfectly in QEMU while the disk is quietly broken for everyone
else. The symptom lives entirely on your host machine.

Which points at the real test.

## The test that actually proves it

Your reader agreeing with your writer proves nothing. They can share a bug and
agree beautifully about a disk that no other program on earth can read.

The test is: write a file from TownOS, quit QEMU, and read it from your host with
`mtype -i disk.img ::/NOTES.TXT`.

`mtools` did not read your code. It implements the format from the spec. If it can
read what you wrote, and if `mdir` reports the volume as undamaged, then you have
written real FAT32 and not merely a private format that happens to resemble it.

There is a second test with the same character. Write a file, quit QEMU, restart,
and read it back. That is what proves you wrote a disk and not a cache.

And a third, which by now should look familiar: write and delete in a loop and
watch the free cluster count. It has to come back to the same number every cycle.
If it drifts down you are leaking clusters. If it drifts **up** you are freeing
clusters that are still in use, which is worse, because the damage has not shown
up yet.

That is the same shape as the free-frame test from the process lifecycle rung. It
turns out to be the shape of every resource test you will ever write.

## What this still is not

Worth being precise, because "writable filesystem" sounds like more than it is.

- **No file handles.** You write a whole file in one call. There is no open, no
  seek, no append, no writing byte 4000 of an existing file without rewriting all
  of it. Handles want a per-process descriptor table, and that wants to be designed
  alongside pipes.
- **No subdirectories.** Still root only, on both the read and the write side. The
  chain walk would handle a subdirectory happily; there is no path parser to tell
  it which one.
- **No timestamps.** The create and write date fields exist in every entry and we
  leave them zero. Filling them needs a clock that knows what year it is, and the
  town has a timer but no calendar.
- **No crash safety beyond ordering.** No journal, no atomic rename, no fsync. If
  the machine dies mid-write you may lose clusters. You will not lose data that was
  already there, and that guarantee comes entirely from rule 4.

## Exercises

1. A file is created and the machine dies immediately after the FAT chain is linked
   but before the directory entry is written. Describe exactly what is on the disk,
   what a later `list` shows, and what a disk checker would report.
2. Now the same crash, but the writer used the wrong order and wrote the directory
   entry first. Describe the disk again, and then describe what happens to a
   *different* file created ten minutes later.
3. Why must `free_chain` read the next pointer before it zeroes the current entry?
   Write down what happens if you do it the other way.
4. Why does `free_chain` need a bounded loop? What state would the disk have to be
   in for it to matter, and what would the symptom look like from the keyboard?
5. Replacing a file allocates the new chain before freeing the old one, so briefly
   both exist. What does that cost, and under what circumstance does that cost
   actually stop you from replacing a file?
6. Clusters are 512 bytes and a directory entry is 32 bytes. How many files can the
   root directory hold before it has to grow? Why is that number lucky for you and
   unlucky for someone testing on a real USB stick?
7. You update only the first FAT copy. List every test you could run inside QEMU
   that would still pass, and then name the one test that would fail.
8. The tail of the last cluster of a file is not zero-filled. The `size` field means
   no reader ever returns those bytes, so nothing is functionally wrong. Explain why
   you should zero it anyway.
9. FSInfo's free-count field is described by the format as a hint that may be wrong.
   Given that, why bother setting it to 0xFFFFFFFF instead of just leaving the stale
   number in place?
