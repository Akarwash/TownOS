# The FAT32 filesystem

This is the factual description of MiniOS's filesystem layer, read from
`fs/fat32.c` and `fs/fat32.h`. It is read-only FAT32: it can list the root
directory and read a file by name. For why read-only, and why 8.3 names and the
first FAT copy only, see
[decisions/0014](../decisions/0014-read-only-fat32.md). For the block device
underneath it, see [disk.md](disk.md).

## What this layer adds

The disk driver hands out numbered 512-byte blocks and knows nothing else. This
layer reads the bookkeeping that the FAT32 format leaves on the disk itself and
turns it into names. The bookkeeping lives at fixed, known places (block 0 first,
and everything else is found from what block 0 says), which is what makes a disk
formatted by one machine readable by another.

Three structures do all the work:

| Structure | Answers |
|-----------|---------|
| The boot sector (BPB) | Where is everything, and how big are the pieces |
| The FAT | Which clusters does this file occupy, and which are free |
| A directory | What is this file called, where does it start, how long is it |

## The on-disk layout

The image is formatted as a "superfloppy": the FAT32 volume starts at block 0
with no partition table, so block 0 of the disk is the boot sector and there is
no partition entry to walk first.

```
block 0                                                    end of volume
|                                                                     |
+----------------+----------+----------+---------------------------+
| reserved area  |  FAT #1  |  FAT #2  |        data area          |
+----------------+----------+----------+---------------------------+
 ^                ^                     ^
 boot sector      first_fat_block       first_data_block = cluster 2
 is block 0
```

- **Reserved area**: `reserved_sector_count` blocks, starting with the boot
  sector. Its length is read from the BPB, not assumed.
- **FAT copies**: `num_fats` identical copies, `sectors_per_fat` blocks each,
  back to back. MiniOS reads the first only.
- **Data area**: everything after, carved into clusters.

On the image `tools/mkdisk.sh` produces (64MB, formatted by `mformat`), those
numbers come out as:

| Field | Value |
|-------|-------|
| Bytes per sector | 512 |
| Sectors per cluster | 1 (so a cluster is 512 bytes) |
| Reserved sectors | 32 |
| Number of FATs | 2 |
| Sectors per FAT | 1009 |
| First data block | 32 + (2 x 1009) = 2050 |
| Root cluster | 2 |
| Total sectors | 131072 |
| Data clusters | 129022 |

The cluster count matters: FAT32 is only legal at 65525 clusters or more, which
is why the image is 64MB and not 16MB.

## Clusters

A cluster is a run of consecutive blocks treated as one unit. The filesystem
thinks in clusters; the driver thinks in blocks. Bigger clusters mean less
bookkeeping and more waste on small files (a 1 byte file still occupies a whole
cluster). The person formatting the disk picks the tradeoff, and the kernel reads
whatever it finds.

Because `sectors_per_cluster` is a single byte on disk, and `disk_read`'s count
is also a `uint8_t`, one cluster is always one `disk_read` call. `fs/fat32.c`
pins that reasoning down with a compile-time guard rather than a comment alone,
since it is the only thing keeping a cluster read to a single call.

## The boot sector: the fields actually used

`fat32_init` reads block 0 into a `struct fat32_bpb` and uses these fields:

| Offset | Field | Used for |
|--------|-------|----------|
| 11 | `bytes_per_sector` | Must be 512; the block size everything is counted in |
| 13 | `sectors_per_cluster` | Cluster size, and the block count of a cluster read |
| 14 | `reserved_sector_count` | Where the first FAT starts |
| 16 | `num_fats` | How many FAT copies to skip to reach the data area |
| 36 | `sectors_per_fat_32` | Length of one FAT copy |
| 32 | `total_sectors_32` | Volume size, used to compute the cluster count |
| 44 | `root_cluster` | Where the root directory begins |
| 510 | `0xAA55` signature | Sanity: is block 0 a boot sector at all |

Two traps live in that table.

**`sectors_per_fat_32`, not `sectors_per_fat_16`.** The 16-bit field at offset 22
is the FAT12/FAT16 one and is zero on a FAT32 volume. Reading it gives a FAT
length of zero, which places the data area on top of the FAT.

**The struct must be packed.** These fields sit at unaligned offsets:
`bytes_per_sector` is a 16-bit field at offset 11, an odd address. Without
`__attribute__((packed))` the compiler inserts padding to align them, and every
field after the first misalignment reads the wrong bytes. The symptom is that the
first parsed field looks right and everything after it is garbage. This is the
same trap as the Multiboot mmap entry. `fs/fat32.c` carries a compile-time guard
(`sizeof(struct fat32_bpb) == 48`) so that padding fails the build rather than
surfacing at runtime.

`fat32_init` sanity-checks what it parsed (512-byte sectors, a power-of-two
cluster size, non-zero FAT count and reserved area, a non-zero 32-bit FAT length,
a root cluster of at least 2, a volume bigger than its own metadata) and returns
-1 with a printed message rather than computing block numbers from garbage.

## Cluster to block

```
first_data_block   = reserved_sector_count + (num_fats * sectors_per_fat)
block_of_cluster(n) = first_data_block + ((n - 2) * sectors_per_cluster)
```

**Why the `- 2`.** FAT slots 0 and 1 are reserved by the format. They hold flags,
not file data, so the first cluster that can hold anything is cluster 2, and
cluster 2 therefore sits at offset 0 of the data area. Cluster 3 sits one cluster
in, and so on.

This is the single easiest thing to get wrong here, and it fails loudly in a
specific way: **file contents come back shifted by exactly two clusters' worth of
bytes.** Removing the subtraction as a negative control turned the root listing
into a garbage entry name and failed both file reads, which is exactly the
expected symptom.

## The FAT: entries, the 28-bit mask, and chains

The FAT is a flat array of 32-bit entries, one per cluster, starting at
`first_fat_block`. Finding cluster N's entry is a division: block
`first_fat_block + (N / entries_per_block)`, byte offset `(N % entries_per_block)
* 4` inside it. Entries are little-endian and their offsets need not be 4-byte
aligned, so `fs/fat32.c` assembles each one byte by byte rather than casting
through a `uint32_t` pointer.

**Only the low 28 bits are meaningful.** The top four bits are reserved and may
hold anything the formatter left there, so every entry is masked with
`0x0FFFFFFF` before it is compared to anything. Skipping the mask is the classic
FAT32 bug, and its symptom is nasty: end-of-chain detection fails
*intermittently*, depending on what happens to be in the reserved bits of the
particular entry that ends a particular file.

After masking:

| Value | Meaning |
|-------|---------|
| `0x00000000` | Free cluster |
| `0x0FFFFFF7` | Bad cluster |
| `>= 0x0FFFFFF8` | End of chain (a range, since formatters differ) |
| anything else | The number of the next cluster in this file |

So a file is a chain, not a run: the directory says where it starts, and each
cluster's slot names the next one. This is what lets a file's clusters be
scattered across the disk (fragmentation) without complicating the read at all;
following a pointer to a distant cluster is the same operation as following one
to the neighbouring cluster.

The same table is the free list: a slot holding zero means nothing is using that
cluster. A writer would search for zeros here, and would have to update every FAT
copy. A reader needs only the first copy.

`fat32_next_cluster` reports three outcomes as distinct results (next cluster,
end of chain, error) because "the chain ended" and "the read failed" are
different facts. It also treats a next-cluster value that is free, bad, or
outside the volume as an error rather than following it.

**Chains are bounded.** Both the directory walk and the file read cap the number
of clusters followed at the volume's cluster count, and report an error if they
run past it. A valid chain cannot be longer than that, so exceeding it proves
corruption. This is deliberate: a self-referential chain would otherwise spin
forever, and a filesystem that hangs the machine on bad data is worse than one
that reports an error. Same reasoning as the disk driver's bounded polling.

## Directory entries

A directory is a file. Reading one means following its cluster chain exactly like
any other file. Its contents are a run of 32-byte entries.

| Offset | Field | Notes |
|--------|-------|-------|
| 0 | `name[11]` | 8.3, space-padded, uppercase, no dot stored |
| 11 | `attr` | The attribute bits below |
| 20 | `first_cluster_high` | High 16 bits of the start cluster |
| 26 | `first_cluster_low` | Low 16 bits of the start cluster |
| 28 | `size` | File length in bytes |

The first byte of an entry doubles as a marker, and the two cases mean different
things:

- `0x00`: this entry was never used, and neither is anything after it. This is
  the end of the directory, so scanning stops.
- `0xE5`: this entry was used and the file was deleted. It is a hole. Scanning
  skips it and continues, because live entries follow.

Attribute bits:

| Bit | Meaning | Handling |
|-----|---------|----------|
| `0x01` | Read only | Ignored |
| `0x02` | Hidden | Ignored |
| `0x04` | System | Ignored |
| `0x08` | Volume label | Skipped, it is not a file |
| `0x10` | Subdirectory | Listed as `<DIR>` |
| `0x20` | Archive | Ignored |
| `0x0F` (whole byte) | Long filename fragment | Skipped |

The long-filename check tests the whole attribute byte and must come first: an
LFN entry has the volume-id bit set too, so testing that bit first would
misclassify it as a volume label.

**The start cluster is split in two.** The high half sits at offset 20 and the
low half at offset 26, at opposite ends of the entry, because the high half was
bolted into a gap in the FAT16 layout when FAT32 was defined. They must be
recombined as `(high << 16) | low`. Using only the low half works on small
volumes and then breaks on large ones, producing wildly wrong cluster numbers.

### The 8.3 name encoding

The name field is 11 bytes with no dot stored: 8 bytes of base name then 3 of
extension, both space-padded, uppercase.

```
"HELLO.TXT"  ->  "HELLO   TXT"
"BIG.TXT"    ->  "BIG     TXT"
```

`fs/fat32.c` has a helper each way: one converts a caller's name into the 11-byte
form for comparison (uppercasing as it goes, so lookups are case insensitive, and
rejecting anything that will not fit in 8.3), and one formats the on-disk form
back for display (trimming the padding and putting the dot back, with no trailing
dot when the extension is empty, as it usually is for directories).

Names that cannot be expressed in 8.3 are rejected up front, which is correct
rather than merely convenient: a file with a long name is invisible to this
layer anyway, since its real name lives in the LFN entries that are skipped.

## The read path, end to end

`fat32_read_file("HELLO.TXT", buf, bufsize, &size)`:

1. **Convert the name** to its 11-byte 8.3 form. A name that will not fit is
   rejected here.
2. **Walk the root directory**, following its chain, comparing each usable
   entry's 11-byte name. Not found is -1.
3. **Read the entry**: recombine the start cluster from its two halves, take the
   size. Refuse a directory, and refuse a file larger than the caller's buffer.
4. **Follow the chain**, reading one cluster at a time into a `kmalloc`'d cluster
   buffer (the heap, not the stack, because a cluster can be up to 128KB at the
   format's maximum).
5. **Copy and trim.** Copy whole clusters until the last one, which is usually
   only partly real data, then copy just the remaining bytes. The chain gives
   whole clusters; the directory entry's size is what says where the real data
   ends inside the last one. Without the trim, every file would come back with
   the stale bytes that happened to follow it on disk.
6. **Stop** at end-of-chain or when `size` bytes have been delivered, whichever
   comes first. A chain that ends before `size` bytes is a corrupt volume and
   returns -1 rather than a short read the caller would mistake for the whole
   file.

Steps 1 through 3 are pure bookkeeping and only step 4 onward touches file data.
That ratio is normal; filesystems spend most of their effort finding things.

## The interface

```c
int fat32_init(void);
int fat32_list_root(void);
int fat32_read_file(const char *name, void *buf, uint32_t bufsize,
                    uint32_t *out_size);
```

`fat32_init` must be called once, after `disk_init`, and returns -1 if the disk
is unreadable or does not hold a FAT32 volume it can trust. `fat32_list_root`
prints each entry's name and either its size in bytes or `<DIR>`.
`fat32_read_file` takes an 8.3 name (case insensitive) from the root directory,
fills `buf`, writes the real byte count to `out_size`, and returns -1 if the name
is not 8.3, is not found, names a directory, does not fit in `bufsize`, or the
volume is corrupt.

## What this layer does not do

- **No writing.** Nothing can be created, modified, or deleted; the disk's
  contents are a build input, produced by `tools/mkdisk.sh` on the host.
  `TODO(fat32-write)`.
- **No long filenames.** LFN entries are skipped, so a file with a long name is
  invisible to MiniOS even though the host can see it.
- **No paths.** Lookups are root-directory only. The walk takes any starting
  cluster and would read a subdirectory's entries, but the interface has no path
  to split.
- **No caching.** Every FAT lookup reads a full 512-byte block, so a file
  spanning N clusters costs about N data reads plus N chain reads. Correct and
  slow, and since every read goes through the polled driver, reading a large file
  freezes the machine for its duration.
- **No permissions and no crash safety.** FAT32 has essentially neither. See the
  ADR.

## Where to read more

- The decision and its scope: [decisions/0014](../decisions/0014-read-only-fat32.md)
- The block device underneath: [disk.md](disk.md)
- Building the image, and adding files to it: [../building.md](../building.md)
- The concepts behind all of this:
  [`../../learnings/16-the-filesystem.md`](../../learnings/16-the-filesystem.md)
