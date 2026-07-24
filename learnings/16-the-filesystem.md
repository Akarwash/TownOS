# Chapter 16: The Filesystem

> Read chapter 15 (the disk driver) first. This chapter is the layer directly on
> top of it, and it only makes sense once you accept how little the disk does.

## Where we are

You have a disk driver. Ask it for block 5, it gives you 512 bytes. Tell it to
write block 5, it overwrites whatever was there.

That is the entire vocabulary. The disk understands exactly one noun (a numbered
block) and two verbs (read, write).

It does not know what a file is. It does not know which blocks are in use. It
will happily let you overwrite something important, because it has no idea
anything is important. It is a wall of numbered lockers with no labels and no
front desk.

A filesystem is the front desk.

## The three questions

Say you want to save something called `notes.txt`. To do that, somebody has to
answer three questions, and the disk can answer none of them.

1. **Which blocks are free?** If you just pick block 5, you might land on top of
   something already there. Something has to track what is taken.
2. **Which blocks did this file end up in?** You will want it back later. Something
   has to remember where it went.
3. **What is it called?** You want to ask for `notes.txt`, not for "block 5". Something
   has to hold that name.

That is it. That is the whole job. A filesystem is a scheme for answering those
three questions, nothing more.

## The big idea

Here is the move that makes it work, and it is worth sitting with because it
sounds circular at first.

**The answers are stored on the disk itself.**

The list of which blocks are free? Written into some blocks. The record of which
blocks belong to which file? Written into some blocks. The names? Written into
some blocks.

The bookkeeping lives inside the thing it describes.

That sounds like a snake eating its tail, but it is fine, because the bookkeeping
lives at **known, fixed places**. The filesystem reserves the first handful of
blocks for itself. When you boot, you read those, and now you know where
everything else is. The map is buried in the territory, but it is always buried
in the same spot.

This is why a disk formatted on your laptop works on someone else's. The format
is an agreement about *where the bookkeeping lives and what shape it is*. Both
machines read the same blocks and find the same map.

## First, clusters

One small thing before the main idea.

512 bytes is a small unit. A disk has millions of blocks, and tracking every one
individually means a huge amount of bookkeeping. So filesystems group blocks into
slightly bigger chunks and track those instead.

A **cluster** is a small run of consecutive blocks glued together, treated as one
unit. If a cluster is 4 blocks, then cluster 0 is blocks 0 to 3, cluster 1 is
blocks 4 to 7, and so on.

Everything from here on is in clusters, not blocks. The filesystem thinks in
clusters. The driver thinks in blocks. Converting between them is simple
multiplication, and we will do it explicitly later.

The cost of bigger clusters: a 10 byte file still consumes a whole cluster. The
benefit: less bookkeeping. It is a straight tradeoff, and the person formatting
the disk picks where to land.

## The File Allocation Table

FAT stands for **File Allocation Table**. It is one table, and it does the heavy
lifting for two of the three questions.

Picture one enormous array. One slot per cluster on the disk. Cluster 0 gets slot
0, cluster 1 gets slot 1, forever.

Each slot holds **one number**. And that number means:

> "After this cluster, the file continues in cluster N."

That is all a slot says. It is a forwarding address.

So a file is not stored as "clusters 5, 6, 7". It is stored as a **treasure
hunt**:

- Something tells you the file starts at cluster 5.
- You go to slot 5. It says 9. So the next piece is in cluster 9.
- You go to slot 9. It says 2. Next piece is cluster 2.
- You go to slot 2. It says **END**. Stop, that was the last piece.

The file is clusters 5, then 9, then 2. Each slot points at the next one. The
data is scattered across the disk, but the trail of pointers is all gathered in
one table.

That is a linked list, if you have seen one. The difference from a normal linked
list is that the pointers do not live next to the data. They all live together in
the FAT.

### The free question, answered by the same table

Now the small piece of cleverness.

A slot holding **0** means "this cluster is empty". Nothing is using it.

So "find free space" is not a separate system. It is: **scan the FAT for a slot
holding 0.**

One table, two jobs. It is the chain-map for files, and it is the free list, at
the same time, because "belongs to a file" and "is free" are just different values
in the same slot.

### The directory answers the third question

The FAT knows nothing about names. Slot 5 does not know it is the start of
`notes.txt`.

That is what a **directory** is for, and a directory is less magical than it
sounds. A directory is a file. Its contents are a list of entries, and each entry
says roughly:

```
name          starting cluster      size in bytes
notes.txt     5                     300
report.txt    3                     5000
```

That is a folder. A file whose contents are a list of "name to starting cluster"
pairs. When you open a folder in a file browser, something read that list and drew
it for you.

And because a directory is a file, a directory can contain another directory, and
that is how folders nest. There is no special "folder machinery". It is files all
the way down.

## Three worked examples

Small disk. 10 clusters. The FAT has 10 slots.

`0` means free. A number means "next cluster is". `END` means last cluster of a
file.

Starting state, empty disk:

```
cluster:  0    1    2    3    4    5    6    7    8    9
FAT:    [ - ][ - ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ]

directory: (empty)
```

Slots 0 and 1 are marked `-` because the format reserves them. They never hold a
file. We will come back to why.

### Example 1: a small file that fits in one cluster

Save `notes.txt`, 300 bytes. Say a cluster holds 2048 bytes. It fits in one.

1. Scan the FAT for a `0`. First free is slot 2. Take it.
2. Write the 300 bytes into cluster 2, using the disk driver.
3. Mark slot 2 as `END`. This file stops here.
4. Add a directory entry: `notes.txt` starts at cluster 2, size 300.

```
FAT:    [ - ][ - ][END][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ][ 0 ]
                    ^

directory: notes.txt -> start 2, size 300
```

Reading it back: the directory says start at 2. Read cluster 2. Check slot 2, it
says END. Done. One cluster, one read.

### Example 2: a bigger file that needs a chain

Save `report.txt`, 5000 bytes. At 2048 bytes per cluster, that needs 3 clusters
(2048 times 2 is 4096, not enough; times 3 is 6144, enough).

1. Scan for free clusters. Finds 3, 4, 5.
2. Write the data across clusters 3, 4, and 5.
3. Link them: slot 3 holds `4`, slot 4 holds `5`, slot 5 holds `END`.
4. Directory entry: `report.txt` starts at cluster 3, size 5000.

```
FAT:    [ - ][ - ][END][ 4 ][ 5 ][END][ 0 ][ 0 ][ 0 ][ 0 ]
                        ^    ^    ^
                        chain: 3 -> 4 -> 5 -> END

directory: notes.txt  -> start 2, size 300
           report.txt -> start 3, size 5000
```

Reading it: start at 3. Read cluster 3. Slot 3 says next is 4, read cluster 4.
Slot 4 says next is 5, read cluster 5. Slot 5 says END. Stop.

You followed the trail. Three reads, guided by three FAT lookups.

### Why the size field matters

Three clusters is 6144 bytes of space. The file is 5000 bytes. So the last
cluster is only partly real data, and the remaining 1144 bytes are leftover
garbage from whatever was there before.

The directory's **size** field is what stops you. You read three full clusters,
then hand back only the first 5000 bytes.

Without the size, you would return 1144 bytes of junk on the end of every file
and never know. The FAT tells you *where* the data is. The size tells you *where
it ends*.

### Example 3: delete, then a fragmented file

This is the one worth understanding properly.

**Step A: delete `notes.txt`.**

What actually happens? Two things:

- Mark its FAT slot as free (set slot 2 back to `0`).
- Mark the directory entry as deleted.

That is all. **The data in cluster 2 is not erased.** It is still sitting there,
byte for byte.

```
FAT:    [ - ][ - ][ 0 ][ 4 ][ 5 ][END][ 0 ][ 0 ][ 0 ][ 0 ]
                    ^ freed, but cluster 2 still physically holds the old bytes

directory: report.txt -> start 3, size 5000
```

Two consequences fall out of this, and both are things you have probably run into
without knowing why.

**Deleting is instant, regardless of file size.** Deleting a 40 gigabyte file
takes the same time as a 300 byte one, because you are editing the map, not the
territory. You changed a few numbers.

**Deleted files are often recoverable.** The bytes are still on the disk. Recovery
tools work by scanning for data whose bookkeeping was removed but whose contents
were never overwritten. This is also why "securely erasing" a drive is a separate,
slow operation: it has to actually write over the data, which normal deletion
never does.

**Step B: now save `data.bin`, also 5000 bytes, so 3 clusters.**

Which clusters are free now? Slot 2 (just freed), plus 6, 7, 8, 9.

Scanning from the beginning, the first three free are **2, 6, and 7**.

Notice what that means. Cluster 2 sits *before* clusters 3, 4, 5, which belong to
`report.txt`. Clusters 6 and 7 sit *after*. So the new file is split around a file
that was already there.

```
FAT:    [ - ][ - ][ 6 ][ 4 ][ 5 ][END][ 7 ][END][ 0 ][ 0 ]
                    ^                   ^    ^
                    data.bin:   2 -> 6 -> 7 -> END
                    report.txt: 3 -> 4 -> 5 -> END

directory: report.txt -> start 3, size 5000
           data.bin   -> start 2, size 5000
```

`data.bin` physically lives in clusters 2, 6, and 7, with somebody else's file
sitting in the gap. That is **fragmentation**.

### And here is the payoff of the whole design

Reading `data.bin` works **exactly the same as before**. Start at 2. Slot 2 says
6. Slot 6 says 7. Slot 7 says END.

The chain does not care that the clusters are not next to each other. It never
did. Following a pointer to cluster 6 is the same operation whether cluster 6 is
adjacent or on the other side of the disk.

**This is why the FAT is a chain and not a length.**

Imagine the simpler design: store "starts at cluster 3, is 3 clusters long". No
table needed. Much simpler.

But then every file must be **contiguous**. And now delete `notes.txt` and you
have left a one cluster hole that only a file of exactly that size or smaller can
ever use. Do that a few hundred times and your disk is swiss cheese: plenty of
free space, none of it in a usable run. You would have to constantly shuffle
files around to make room.

The chain buys you the freedom to use any scattered set of clusters. That freedom
is worth an entire table.

The cost is real though. On a spinning disk, reading a fragmented file means the
read head physically jumps around, which is slow. That is literally what
"defragmenting" was: rewriting files so their chains run in order again. On an SSD
there is no head to move, so fragmentation barely matters and defragmenting is
pointless.

## The real FAT32, the parts you will meet in code

Everything above is the idea. Here are the specific details you will actually run
into, each of which will look arbitrary until you know what it is for.

### The boot sector, where the filesystem describes itself

Block 0 of the disk is special. It holds a structure describing the filesystem's
own shape:

- how many bytes in a block (basically always 512)
- how many blocks in a cluster
- how many blocks are reserved before the FAT starts
- how many copies of the FAT there are
- how many blocks each FAT occupies
- which cluster the root directory starts at

This is the map to the map. You read block 0 first, always, before anything else.
Everything after depends on those numbers.

It has a formal name (the BIOS Parameter Block, or BPB) but the name does not
matter. It is the header. It is the filesystem telling you its own dimensions so
you can find everything else.

### Turning a cluster number into a block number

The driver only speaks blocks. The filesystem only speaks clusters. So every read
needs a conversion, and it is just arithmetic:

```
first_data_block = reserved_blocks + (number_of_FATs * blocks_per_FAT)

block_of_cluster(n) = first_data_block + ((n - 2) * blocks_per_cluster)
```

Read that top line as a layout. The disk goes: reserved blocks, then the FAT
(possibly a few copies of it), then the actual data area. So you skip past the
first two regions to find where data begins.

### Why the minus 2

That `(n - 2)` looks like a bug. It is not.

Remember slots 0 and 1 in our examples, the ones marked `-`. The format reserves
those two slots. They hold flags, not file data. So the first cluster that can
actually hold a file is **cluster 2**.

Which means cluster 2 is the *first* cluster of the data area, so it sits at
offset 0 in that area. Cluster 3 sits at offset 1. Hence `n - 2`.

It is a wart. It exists because the first two FAT slots were spent on something
else decades ago, and the numbering never got cleaned up. Expect to get this wrong
once. Everyone does. If your file reads come back shifted by exactly two clusters
worth of bytes, this is why.

### Why there are two copies of the FAT

The FAT is the single most important structure on the disk. Lose it and you have
a disk full of data with no idea which bytes belong to what. The data is all still
there, and it is all useless.

So the format keeps **two identical copies**, back to back. If one is damaged, the
other can rebuild it.

For reading, you only ever need the first one. For writing, you must update
**both**, or they disagree and the disk is corrupt. This is one of several reasons
write support is meaningfully harder than read support.

### The root directory

Every directory is a file, including the top level one. The boot sector tells you
which cluster the root directory starts at. From there it is an ordinary chain,
read exactly like any other file.

The contents are a run of fixed size entries, one per file, each holding the name,
the starting cluster, the size, and some flags (is this a directory, is it
read-only, and so on).

### The name problem

The original format allowed 8 characters plus a 3 character extension, stored in a
fixed 11 byte field, uppercase, space padded. `notes.txt` is stored as
`NOTES   TXT`. That is the "8.3" name.

Longer names were bolted on later by chaining together extra directory entries
that the old software would ignore. It works, and it is genuinely unpleasant to
parse.

For a first implementation, handle 8.3 names only and skip the long name entries.
You lose nothing important, and you avoid a large pile of fiddly parsing that
teaches you nothing new about filesystems.

## The full read path, end to end

Putting it together. To read `HELLO.TXT`:

1. Read block 0. Parse the boot sector. Now you know cluster size, where the FAT
   starts, where data starts, and which cluster the root directory is in.
2. Read the root directory (follow its chain, same as any file).
3. Scan its entries for the name `HELLO   TXT`. Read off its starting cluster and
   its size.
4. Start at that cluster. Convert cluster to block. Read it.
5. Look up that cluster's slot in the FAT. If it holds a next cluster, go there
   and repeat. If it holds END, stop.
6. You now have a pile of whole clusters. Trim it to the size from the directory
   entry. That is the file.

Six steps. Every one of them is either a disk read or a lookup in something you
already read.

Notice that steps 1, 2, and 3 are pure bookkeeping, and only step 4 onward touches
the actual file data. That ratio is normal. Filesystems spend most of their effort
finding things.

## Why read-only first

Reading needs: parse the boot sector, follow chains, parse directory entries.

Writing needs all of that, plus: find free clusters, update the chain correctly,
update **both** FAT copies, update the directory entry, extend a directory when it
fills up, and get the ordering right so a crash midway does not leave the disk in
a state where the FAT and the directory disagree.

Writing is where filesystems get genuinely hard, and where a bug corrupts data
instead of just failing.

There is also a practical reason to do reads first, and it is the strong one:
**reading is the complete prerequisite for loading a program off the disk.** You
read a program image, you never write it. So a read-only filesystem is not half a
step toward the next rung, it is the whole thing that rung needs.

Write support (saving a file from the shell) is a real feature, and it is its own
separate piece of work.

## What a filesystem still is not

Worth being precise, because the word gets used loosely.

A filesystem gives you names, free space tracking, and a way to find a file's
bytes. It does not give you:

- **permissions.** FAT32 has almost none. No owners, no per user access. That is a
  property of fancier formats.
- **crash safety.** If the machine dies halfway through a write, FAT32 can be left
  inconsistent. Journaling filesystems solve this by writing down what they are
  about to do before doing it. FAT32 does not.
- **any notion of a running program.** A file is bytes with a name. Turning a file
  into a running process is a separate job (a loader), and it is the next rung
  after this one.

## Exercises

Answer these before reading the code. They are all answerable from this chapter.

1. A file is 1 byte. Clusters are 2048 bytes. How much disk space does it consume,
   and why?
2. You delete a 40 gigabyte file and it happens instantly. Explain exactly what
   was changed on the disk, and what was not.
3. The FAT is one table doing two jobs. Name both, and explain how a single slot
   value distinguishes them.
4. Why does `block_of_cluster` subtract 2? What breaks if you forget it, and what
   would the symptom look like?
5. A file's chain is 4 clusters long and the size field says 3 bytes. Is the
   filesystem corrupt, or is this legal? Explain.
6. Why does the format keep two copies of the FAT, and why does read-only support
   only need one of them?
7. You have a design where a directory entry stores "start cluster and length in
   clusters", with no FAT at all. Describe a sequence of file creations and
   deletions that leaves the disk with plenty of free space but unable to store a
   3 cluster file.
