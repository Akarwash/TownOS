# Chapter 15: The Disk Driver

> Read chapter 4 (device drivers and I/O) first. This is a fourth driver, built on
> the same two primitives, and it is the last layer before things start having
> names.

## Where we are

Everything built so far forgets.

Turn the machine off and the kernel is gone, the tasks are gone, the heap is gone,
every byte of it. RAM is a thing that holds values while you keep paying for
electricity.

You want something that does not do that. That is the whole subject.

## Rule 1: a disk is a flat array of numbered boxes

Here is the entire model the hardware offers.

The disk is a sequence of fixed-size blocks, numbered from zero. On this platform a
block is 512 bytes. Block 0 is the first 512 bytes, block 1 the next 512, and so on
to the end.

That is it. There are no files. No names. No directories. No sizes. No notion of
which blocks are in use and which are empty. No way to ask "where did I put that."

You can read block 4,182,663 and you can write block 4,182,663, and the disk has no
opinion about whether either was a good idea.

**A disk is a wall of numbered lockers with no labels and no front desk.**

## Rule 2: why numbers rather than names

Worth asking, because "give it a name" seems so obviously more useful.

Because a name is a convention, and a convention has to be *interpreted* by
somebody, and the disk is not somebody. It is a mechanism. Mechanisms count.

Every property you actually want, names, sizes, directories, knowing which space is
free, is a fiction maintained in software by writing bookkeeping into some of the
blocks and agreeing on how to read it back. There is nothing on the platter that
knows it is a filesystem. There are only blocks, some of which happen to contain
notes about the others.

That layer is chapter 16, and keeping the boundary sharp is the point of this
chapter. **This driver names exact blocks. The filesystem chooses them.**

## Rule 3: how the bytes actually move

Two families of answer, and the difference matters more than it looks.

**Programmed I/O (PIO).** The CPU personally moves every word, reading or writing it
through an I/O port. The CPU is the courier and does the carrying itself.

**Direct memory access (DMA).** The CPU tells the device where in RAM the data
should go and then walks away. The device performs the transfer itself, into memory,
while the CPU does something else entirely, and raises an interrupt when it is done.

TownOS uses PIO. The CPU moves all 256 words of every block by hand.

## Rule 4: and it polls, which means a disk read stops the machine

This is the sentence to take away from the chapter.

The driver writes a command to the drive and then **spins**, reading a status port
over and over, until the drive says it is ready. Then it moves the words, one at a
time, itself.

While that is happening, nothing else on the machine runs. Not the other tasks. Not
the shell. Nothing. The CPU is in a loop reading a port.

And a disk is slow. RAM answers in nanoseconds; a disk answers in milliseconds. That
is a factor of about a million. During a single block read, the CPU could have
executed millions of instructions. It executed a spin loop instead.

Now notice what that is. It is a **busy-wait**, exactly the shape chapter 19 goes to
some trouble to eliminate for the keyboard, and here it is again, untouched, in the
most expensive place available.

The fix is the same shape as chapter 19's: let the drive raise an interrupt when it
is ready, block the calling task on a `WAIT_DISK` reason, and let the machine get on
with other work in the meantime. The waker would be the disk interrupt, because the
thing that causes an event is the thing that wakes the waiters.

It has not been done. It is worth knowing that the machinery to do it already exists
and the driver simply predates it.

## Rule 5: why choose the slow one anyway

The honest defence, because it is a good decision and not just a shortcut.

**No interrupt handler.** No race between issuing the command and the completion
arriving, no partially-transferred state to hold across a switch, no question of
what happens if a second request arrives during the first.

**No DMA setup.** No handing physical addresses to a device, no worrying about
whether the memory is contiguous, no cache coherence questions.

**It is about a hundred lines and it works.**

And the interface it exposes, read block N, write block N, is **exactly the same
interface a fast driver would expose**. So the whole of chapters 16 and 21 sit on
top of it without knowing or caring, and replacing it later changes nothing above.

The general rule, which is worth more than the driver: **when the layer above is the
interesting one, build the layer below in the dumbest way that works, as long as the
interface is right.** Getting the interface right is what buys you the option to
replace it. Getting the implementation clever buys you very little and costs you the
time you wanted to spend upstairs.

## Rule 6: the shape of every ATA operation

The drive is talked to entirely through a fixed block of I/O ports, and three status
bits drive everything:

- **BSY**: the drive owns the registers. Do not touch anything.
- **DRQ**: a block is ready to move.
- **ERR**: the last command failed.

And every operation is the same sequence:

1. Wait for BSY to clear.
2. Write the block number and the count into the address registers.
3. Write the command.
4. Wait for DRQ.
5. Move 256 words through the data port.
6. Check ERR.

Which is chapter 4's shared driver skeleton again: a transport for bytes, and a way
to know when. The keyboard learned "when" from an interrupt. This one asks
repeatedly.

## Rule 7: two traps that are easy to hit and hard to see

**The data port is 16 bits wide.** A 512-byte block is **256 words**, not 512 bytes.
Write a loop that moves 512 units and you move twice the data, desynchronise the
drive from the driver, and everything afterwards is subtly wrong. This is the
classic ATA PIO bug and it does not announce itself, it just makes the disk contain
plausible-looking garbage.

**Reading the status port has a side effect.** Reading status at 0x1F7 acknowledges
a pending interrupt. There is a second port, 0x3F6, that returns exactly the same
bits and does nothing else. So any read that is just a delay or a settle has to use
the alternate one.

That second trap generalises well beyond disks: **some reads are not free.** In
hardware, looking at a thing can change it, and the register you use to observe with
is a decision, not a formality.

## Rule 8: where the block number goes, and the limit it implies

The block number is 28 bits and no register is 28 bits, so it is split across four
ports, with the top four bits sharing a byte with the drive selector.

Send one of those bytes to the wrong port and you read or write **the wrong block,
silently**. No error, no fault, no complaint. You asked for a block and you got a
block, just not the one you meant. On a read that is confusing. On a write it is
destruction.

And 28 bits is a limit: 2^28 blocks of 512 bytes is 128 gigabytes. That was an
absurd amount of storage when the scheme was designed and stopped being absurd
about twenty years later, which is why LBA48 exists.

Worth noticing as a pattern. Every address width in computing has been chosen to be
comfortably enormous and then been embarrassing within a working lifetime.

## Rule 9: what persistence costs

Three things change once data outlives the power, and all three shape everything
built on top.

**Speed.** Six orders of magnitude slower than memory. Essentially every design
decision in a filesystem exists to touch the disk less.

**Ordering matters, because half-finished survives.** A sequence of writes to memory
either completes or the machine dies and it all vanishes. A sequence of writes to
disk can be interrupted halfway and the *half* is still there tomorrow. That single
fact is the entire subject of chapter 21.

**Failure is normal and per-block.** Memory does not usually fail one word at a
time. Disks fail one block at a time, routinely. So every operation returns a status
and every caller has to mean it.

## What this still is not

- **One drive.** Primary bus, master only. No slave, no secondary bus.
- **No partitions.** The driver treats the disk as one flat span from block zero.
- **No caching.** Every read goes to the drive, even for a block read a moment ago.
- **No queueing, no reordering.** One request at a time, in the order asked.
- **LBA28 only**, so 128GB.
- **No error recovery.** ERR is reported and that is the end of it. No retry, no bad
  block remapping.

## Exercises

1. The disk offers numbered blocks and nothing else. List every property you would
   need to add to store a file called `notes.txt`, and say where each one would have
   to live.
2. A block read spins the CPU for milliseconds. Describe what the other tasks on the
   machine are doing during that time, and how a user would perceive it.
3. Sketch the interrupt-driven version of this driver, using chapter 19's machinery.
   Name the new wait reason and say what wakes it.
4. The driver is deliberately simple, and the argument is that the interface is what
   matters. Give an example of a driver interface choice that would have made it
   hard to replace later.
5. The data port is 16 bits. Describe exactly what a 512-iteration loop does, and
   why the resulting corruption would be hard to attribute.
6. Reading status at 0x1F7 acknowledges an interrupt; reading 0x3F6 does not. Give a
   concrete bug that using the wrong one would cause.
7. Sending an LBA byte to the wrong port silently accesses the wrong block. Design a
   test that would catch this, given that neither a read nor a write reports an error.
8. LBA28 caps the disk at 128GB. Find two other address widths in this kernel that
   are comfortably enormous today, and say what would exhaust each one.
