# 0013 - A polled ATA PIO disk driver

## Status

Accepted.

## Context

MiniOS has no way to touch persistent storage. Everything it runs is baked into
the kernel image and everything it computes lives in RAM and is gone at power
off. [project-status.md](../project-status.md) names a filesystem and program
loading as absent by design, and both need the same thing first: a way to read
and write blocks on a disk. There is no block device underneath them to build on.

A disk, at the lowest level, is a flat array of fixed-size blocks addressed by
number: block 0, block 1, block 2, and so on. On this platform a block is 512
bytes. The disk finds nothing and manages nothing. Every read and every write
names an exact block by its number. Choosing which block holds which file, and
tracking which blocks are free, is a filesystem's job, a layer above this one.
The driver is only the mechanism that moves the bytes of a named block.

The emulated hardware is a legacy ATA (IDE) controller. Its primary bus answers
at a fixed set of I/O ports (0x1F0 through 0x1F7, plus 0x3F6). There are three
ways to move the bytes: polled PIO (the CPU reads or writes every word through
the data port and spins on the status port between blocks), interrupt-driven PIO
(the same, but the drive raises IRQ14 when a block is ready instead of the CPU
spinning), and DMA (the drive copies straight to memory and interrupts once at
the end). They rise in speed and in complexity together.

## Decision

Write a polled PIO driver for the primary bus master, using LBA28 addressing.
`drivers/disk.c` exposes `disk_init`, `disk_read(lba, count, buf)`, and
`disk_write(lba, count, buf)`, each moving 512-byte blocks. A new
`port_word_out` in `drivers/ports.c` pushes 16-bit words to the data port, the
write-side mirror of the existing `port_word_in`.

Polled PIO is the deliberate choice. It is the simplest driver that is correct:
the CPU personally copies every one of the 256 words in a block through the data
port and spins reading the status port until the drive says the next block is
ready. No interrupt handler, no DMA descriptors, no shared state between an
interrupt and the caller. It is the right one to learn from and the right one to
build a filesystem on first, before speed matters.

Because it polls, the driver actively silences the drive's interrupt line: it
sets the nIEN bit in the device control register (0x3F6) in `disk_init`, so the
drive never asserts IRQ14. A polled driver that also let the drive interrupt
would raise an IRQ that nothing is waiting on.

LBA28 addressing splits the 28-bit block number across four registers: the low
three bytes go to the LBA-low, LBA-mid, and LBA-high ports, and the top four bits
go into the low nibble of the drive/head port (which also selects the master and
enables LBA mode). READ SECTORS (0x20) and WRITE SECTORS (0x30) start a transfer;
CACHE FLUSH (0xE7) after a write forces the drive to commit its buffer.

Every poll loop is bounded by a large iteration cap and returns -1 on timeout. A
missing disk, or a disk on the wrong bus, must fail loudly rather than hang the
machine forever. This is deliberate, not defensive habit: an unbounded poll
against absent hardware is the failure mode that turns a wiring mistake into a
freeze with no output.

## Consequences

- **The kernel can read and write persistent blocks.** `disk_read` and
  `disk_write` move any run of contiguous 512-byte blocks between a disk and a
  buffer. This is the block device a filesystem needs; it unblocks the filesystem
  and, after that, program loading.

- **A transfer freezes the whole machine.** Because the CPU spins in the poll
  loops with the driver holding the flow, nothing else runs during a transfer,
  and that includes the scheduler: the timer tick cannot preempt a task while a
  block is moving. This is slow and it blocks. It is an accepted limitation of
  polled PIO for a learning kernel, not a bug. The real fix is interrupt-driven
  transfer (the drive raises IRQ14 when a block is ready, the CPU does other work
  meanwhile) and then DMA. Recorded as future work.

- **This is a raw block device, nothing more.** The driver names an exact block
  and moves its bytes. It has no notion of names, files, directories, or free
  space. A caller that writes block 10 and later block 10 again overwrites the
  first write; nothing stops it and nothing tracks it. Choosing blocks and giving
  them names is the filesystem layer, still absent.

- **A write flushes; a read does not.** A write is not durable until the drive
  commits its internal buffer to the platter, so `disk_write` issues CACHE FLUSH
  and waits for it before returning. A read changes nothing on the disk, so it
  needs no flush.

- **No disk IRQ ever fires.** With nIEN set, the primary ATA line (IRQ14, vector
  0x4E in the [self-describing map](0005-self-describing-vector-map.md)) stays
  quiet. Verified under QEMU with `-d int`: no `v=4e` appears, only the timer
  (`v=40`) and syscall (`v=50`) vectors.

- **Verified by round-trip, not by eye.** A disk read is invisible, so the driver
  was proven with a temporary self-test in `kernel_main` (added, verified,
  removed): write a known pattern, read it back into a zeroed buffer, and compare
  byte for byte, for one block and for two contiguous blocks. Both printed `DISK
  TEST: PASS`, with no page (`0x0E`), GP (`0x0D`), or double (`0x08`) fault, and
  the three ring-3 tasks kept interleaving afterward.

## Related

- The port I/O helpers this extends: `drivers/ports.c` (`port_word_out` added
  alongside `port_word_in`).
- The absent layers this unblocks: a filesystem and program loading, in
  [project-status.md](../project-status.md).
- The vector map the silenced IRQ14 belongs to:
  [0005](0005-self-describing-vector-map.md).
- Reference page: [../reference/disk.md](../reference/disk.md).
