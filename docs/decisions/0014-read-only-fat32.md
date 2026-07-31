# 0014 - A read-only FAT32 filesystem

## Status

Accepted. The read-only scope is superseded by
[0020](0020-writable-fat32.md): the filesystem is now read/write. What 0020 carries
forward unchanged still stands — 8.3 names only, the root directory only, and reads
served from the first FAT copy. What it changes is that the volume can now be
written, and a write updates *every* FAT copy, not just the first. The read-side
design described in this ADR is unchanged; for the system as it is now see
[reference/fat32.md](../reference/fat32.md).

## Context

MiniOS has a block device and nothing built on it.
[0013](0013-ata-pio-disk-driver.md) added `drivers/disk.c`, which reads and
writes 512-byte blocks named by number. That is the whole vocabulary: one noun (a
numbered block) and two verbs. The disk does not know what a file is, does not
know which blocks are in use, and will happily let a caller overwrite something
important, because nothing on it marks anything as important.

Three questions have to be answered before a disk holds files, and the driver can
answer none of them: which blocks are free, which blocks a given file occupies,
and what that file is called. Answering them is a filesystem's job, and
[project-status.md](../project-status.md) has named that layer as absent since
the driver landed.

The scheme has to be one other machines already agree on, so the image can be
formatted and filled by the host build system rather than by the kernel. FAT32 is
the obvious choice for a learning kernel: it is simple enough to implement
correctly in one file, universally supported, and the structures are small enough
to read in full. Its bookkeeping is one table (the File Allocation Table) holding
one slot per cluster, where the slot value is both "the next cluster in this
file" and, when zero, "this cluster is free", so a single table is the chain map
and the free list at once.

Reading and writing are not the same size of problem. Reading needs three things:
parse the boot sector, follow cluster chains, parse directory entries. Writing
needs all of that plus free-cluster search, chain updates, updating every FAT
copy, directory entry updates, growing a directory when it fills, and ordering
the writes so a crash midway cannot leave the FAT and the directory disagreeing.
Writing is where filesystems get genuinely hard, and where a bug corrupts data
instead of merely failing.

## Decision

Implement read-only FAT32 in a new `fs/` layer (`fs/fat32.c`, `fs/fat32.h`),
sitting above the disk driver. Three calls: `fat32_init` parses the boot sector
and caches the geometry, `fat32_list_root` prints the root directory, and
`fat32_read_file` reads a named file into a caller's buffer.

Read-only is the deliberate scope, and it is not half a step. Reading is the
complete prerequisite for the next rung, loading a program off the disk, because
a program image is read and never written. Write support is separate future work,
recorded in the source as `TODO(fat32-write)`.

Three further restrictions, each to keep the layer small enough to be read as
learning material:

- **8.3 names only.** Long filenames were bolted onto the format later as chains
  of extra directory entries carrying an attribute byte (`0x0F`) that old
  software ignores. Those entries are skipped. A name is at most 8 characters
  plus a 3 character extension, stored space-padded and uppercase in an 11-byte
  field with no dot (`HELLO.TXT` is stored as `HELLO   TXT`).
- **The first FAT copy only.** The format keeps `num_fats` identical copies for
  redundancy. A reader needs one. A writer would have to update all of them, or
  they disagree and the volume is corrupt.
- **The root directory only, for lookups.** The directory walk itself takes a
  starting cluster and would read a subdirectory's entries just as happily, and a
  subdirectory shows up in a listing as `<DIR>`, but the interface takes a bare
  name with no path to split, so there is no way to name a file inside one. Path
  lookup is future work.

The image is formatted by the host build system, not by the kernel.
`tools/mkdisk.sh` creates a 64MB raw image, formats it FAT32 with `mformat`, and
copies in files with `mcopy` (mtools, so no `sudo` and no mounting). 64MB rather
than the previous 16MB because FAT32 is only legal with at least 65525 clusters,
which 16MB cannot reach with a sane cluster size: tools either refuse or silently
produce FAT16.

Both the chain walk and the file read are bounded by the volume's cluster count
and report an error instead of continuing. A corrupt or self-referential chain
must fail, not hang the machine, the same reasoning as the disk driver's bounded
poll loops.

## Consequences

- **Files can be read by name.** `fat32_read_file("HELLO.TXT", buf, sizeof buf,
  &size)` returns the file's bytes and its real length. This is what unblocks
  program loading: a loader can now read an image off the disk instead of having
  every program compiled into the kernel.

- **Nothing can be saved.** The filesystem is read-only, so the kernel can
  consume what the host build system puts on the image and produce nothing. A
  shell command that writes a file, a program that persists state, and any notion
  of creating or deleting a file are all out of reach until write support exists.
  Recorded as `TODO(fat32-write)` in `fs/fat32.h`.

- **The disk's contents are a build input.** Adding a file to MiniOS's world
  means running `mcopy` on the host, not doing anything inside the kernel. This
  also makes mtools a build dependency (see [../building.md](../building.md)),
  and it means the image must be deleted to be reformatted, since both the make
  rule and the script skip an image that already exists rather than destroying
  what it holds.

- **Long filenames are invisible.** A file copied onto the image as
  `my-long-name.text` is readable by mtools and by any host OS, and completely
  invisible to MiniOS, because its 8.3 entry is a mangled alias and its real name
  lives in the LFN entries this code skips. Files intended for MiniOS must be
  named in 8.3 form.

- **FAT32 brings essentially no permissions and no crash safety.** There are no
  owners and no per-user access; the attribute byte carries a read-only bit and
  little else. Nothing is journalled, so a machine that died halfway through a
  write would leave the volume inconsistent. Neither matters while the
  filesystem is read-only, and both would matter a great deal to a writer. A
  format with real permissions and crash safety is a different format, not a
  flag on this one.

- **A read is many small reads.** Each FAT lookup reads the whole 512-byte block
  holding the entry, so a file spanning N clusters costs roughly N block reads for
  data and N more for chain lookups. There is no block cache. On this volume (one
  block per cluster) that made the 16KB test file 32 cluster reads plus 32 FAT
  reads. It is correct and slow, and since every one of those reads goes through
  the polled driver, reading a large file freezes the machine for the duration.
  A block cache is the obvious fix and is not built.

- **Verified against known contents, not by eye.** A filesystem read is
  invisible: nothing on screen distinguishes correct bytes from plausible
  garbage. A temporary self-test in `kernel_main` (added, verified, removed)
  printed the parsed geometry, listed the root directory, read `HELLO.TXT` and
  compared it to a known string, read a 16KB file spanning 32 clusters and
  checked every byte against a known repeating pattern, and confirmed a missing
  file returns -1 rather than faulting. All five passed, and a negative control
  (removing the `- 2` from the cluster arithmetic) failed them, which is what
  makes the passes mean something. Under `-d int` only the timer (`v=40`) and
  syscall (`v=50`) vectors appear: no page (`0x0E`), GP (`0x0D`), or double
  (`0x08`) fault, no triple fault, and still no disk IRQ (`v=4e`).

## Related

- The block device this sits on:
  [0013](0013-ata-pio-disk-driver.md), [../reference/disk.md](../reference/disk.md).
- Reference page: [../reference/fat32.md](../reference/fat32.md).
- The rung this unblocks (program loading) and the one it does not (writing):
  [../project-status.md](../project-status.md).
- The heap the cluster buffers come from:
  [0010](0010-kernel-heap-ported-from-p5.md).
