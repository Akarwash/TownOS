# The disk driver

This is the factual description of TownOS's disk driver, read from
`drivers/disk.c`, `drivers/disk.h`, and the `port_word_out`/`port_word_in`
helpers in `drivers/ports.c`. It is a polled ATA PIO driver for the primary bus
master, using LBA28 addressing. For why polled PIO was chosen over interrupts or
DMA, see [decisions/0013](../decisions/0013-ata-pio-disk-driver.md).

## The block model: a flat array of 512-byte blocks

A disk is a flat array of fixed-size blocks, numbered from zero. On this platform
a block is 512 bytes (`DISK_SECTOR_SIZE`). The block number is the Logical Block
Address, the LBA: block 0 is the first 512 bytes, block 1 the next 512, and so on.

Every transfer names an exact block. `disk_read(lba, count, buf)` reads `count`
blocks starting at `lba`; `disk_write(lba, count, buf)` writes them. The driver
does not choose blocks, name them, or track which are in use. It moves the bytes
of the blocks it is told to. Choosing and naming blocks is the filesystem's job,
a layer that sits on top of this one and does not yet exist. Keep the two
straight: the driver names exact blocks, the filesystem will choose them.

## The ports: what each one is for

The primary ATA bus answers at a fixed block of I/O ports. The driver talks to
the drive entirely through them (named constants in `drivers/disk.c`, no bare
numbers):

| Port | Name | On write | On read |
|------|------|----------|---------|
| 0x1F0 | `ATA_DATA` | push a 16-bit data word | pull a 16-bit data word |
| 0x1F1 | `ATA_ERROR` | features | error register |
| 0x1F2 | `ATA_SECTOR_COUNT` | number of blocks to transfer | |
| 0x1F3 | `ATA_LBA_LOW` | LBA bits 0-7 | |
| 0x1F4 | `ATA_LBA_MID` | LBA bits 8-15 | |
| 0x1F5 | `ATA_LBA_HIGH` | LBA bits 16-23 | |
| 0x1F6 | `ATA_DRIVE_HEAD` | LBA bits 24-27 + master/slave + LBA mode | |
| 0x1F7 | `ATA_COMMAND` / `ATA_STATUS` | command | status |
| 0x3F6 | `ATA_DEV_CONTROL` / `ATA_ALT_STATUS` | device control (nIEN) | status, no side effects |

The data port is 16 bits wide: one block is 256 words (`ATA_WORDS_PER_SECTOR`),
not 512 bytes of single-byte reads. Reading or writing 512 times instead of 256
is the classic ATA PIO bug; it moves twice the data and desynchronises the drive.

The status port (0x1F7 on read) carries the bits the driver spins on:

| Bit | Name | Meaning |
|-----|------|---------|
| 0x80 | `ATA_STATUS_BSY` | busy: the drive owns the registers, do not touch them |
| 0x08 | `ATA_STATUS_DRQ` | data request: a block is ready to move |
| 0x01 | `ATA_STATUS_ERR` | error: the last command failed |

Reading the status port at 0x1F7 acknowledges a pending interrupt as a side
effect; reading the alternate status at 0x3F6 returns the same bits with no side
effect, which is why the settle delay uses 0x3F6.

## The LBA28 split

The 28-bit block number does not fit in one 8-bit register, so it is split across
four:

```
 bits 0-7  -> ATA_LBA_LOW   (0x1F3)
 bits 8-15 -> ATA_LBA_MID   (0x1F4)
 bits 16-23-> ATA_LBA_HIGH  (0x1F5)
 bits 24-27-> low nibble of ATA_DRIVE_HEAD (0x1F6)
```

The drive/head byte is `ATA_DRIVE_MASTER_LBA` (0xE0, which selects the master and
sets LBA mode) OR'd with the top four LBA bits: `0xE0 | ((lba >> 24) & 0x0F)`.
Sending an LBA byte to the wrong port silently reads or writes the wrong block.

## The read flow

`disk_read` (`ata_prepare` does the shared setup):

1. Poll until BSY clears (`ata_wait_not_busy`): wait for the drive to be idle.
2. Write the drive/head byte: master, LBA mode, top four LBA bits.
3. Settle ~400ns by reading the alternate status port four times and discarding
   the result (see below), so the status bits are valid before they are tested.
4. Write the block count to `ATA_SECTOR_COUNT`.
5. Write LBA bits 0-7, 8-15, 16-23 to the three LBA ports.
6. Write READ SECTORS (0x20) to the command port.
7. For each block: poll until BSY clears and DRQ sets (`ata_wait_for_data`). If
   ERR sets, return -1. Then read 256 words from the data port with
   `port_word_in`.

## The write flow

`disk_write` is the same setup with WRITE SECTORS (0x30), then:

1. For each block: poll until BSY clears and DRQ sets, then push 256 words to the
   data port with `port_word_out`.
2. After all blocks, write CACHE FLUSH (0xE7) and poll until BSY clears.

### Why a write flushes and a read does not

A write is not durable until the drive commits its internal buffer to the platter.
The WRITE SECTORS command may return with the bytes still sitting in the drive's
cache, so `disk_write` issues CACHE FLUSH and waits for BSY to clear, which forces
the commit. A read changes nothing on the disk, so there is nothing to flush.

## Two things that bite

### The 400ns settle

After selecting a drive by writing the drive/head port, the status bits are not
immediately valid; the spec requires roughly 400 nanoseconds to pass first.
Reading the alternate status port (0x3F6) four times and throwing the results
away burns that long with no side effects (`ata_400ns_delay`). Skipping it means
testing BSY/DRQ before they mean anything, which reads a stale value.

### Bounded polling, never infinite

Every poll loop (`ata_wait_not_busy`, `ata_wait_for_data`) is capped at
`ATA_POLL_TIMEOUT` iterations and returns -1 on timeout. This is deliberate. A
disk that is missing, or attached to the wrong bus so 0x1F0-0x1F7 reach nothing,
must fail loudly with -1, not spin forever and freeze the machine with no output.
The cap is large enough for a real transfer to complete yet still terminates.

## Detection and the polled-driver interrupt

`disk_init` does two things:

- It sets the nIEN bit (`ATA_CTRL_NIEN`, 0x02) in the device control register
  (0x3F6). Because this driver polls, it never wants the drive to raise IRQ14;
  nIEN keeps the interrupt line quiet so no disk IRQ fires for no one to handle.
- It does a minimal presence check: select the master, settle, and read the
  status port. A floating (empty) bus reads back 0xFF (`ATA_FLOATING_BUS`); a real
  drive reports something else. It prints whether a disk was detected. A full
  IDENTIFY command is not needed just to know a drive is present.

## The driver-versus-filesystem layering

This driver is the bottom layer: a raw block device. It reads and writes the
512-byte block whose number it is given, and does nothing else. It has no names,
no files, no directories, and no free-space tracking. Writing block 10 twice
overwrites; nothing records that block 10 is in use or what it holds.

The layer above, a filesystem, is what turns names into block numbers: it decides
that a file lives in blocks 10, 11, and 40, keeps a record of which blocks are
free, and hands this driver exact block numbers to move. That layer does not yet
exist. See [../project-status.md](../project-status.md).

## What blocks during a transfer

Because the driver polls, the CPU spins in the wait loops for the whole transfer
and nothing else runs, including the scheduler: the timer tick cannot preempt a
task while a block is moving. A transfer freezes the machine until it finishes.
This is slow and blocking, an accepted limitation of polled PIO. Interrupt-driven
transfer and then DMA are the real fixes, recorded as future work in
[decisions/0013](../decisions/0013-ata-pio-disk-driver.md).
