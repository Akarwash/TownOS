# 0020 - A writable FAT32 filesystem

## Status

Accepted.

## Context

[0014](0014-read-only-fat32.md) built a read-only FAT32 layer and said, plainly,
why it stopped there: writing needs free-cluster search, chain updates, mirroring
every FAT copy, directory-entry creation, growing a directory when it fills, and
ordering the writes so a crash midway cannot leave the FAT and the directory
disagreeing. That is where filesystems get genuinely hard, and where a bug corrupts
data instead of merely failing. Reading was the complete prerequisite for the rungs
that followed (program loading, the shell), and none of them needed to write, so
writing was deferred and marked `TODO(fat32-write)`.

This is that rung. `disk_write` already exists and works
([0013](0013-ata-pio-disk-driver.md)), so nothing new is needed below the
filesystem; everything here is at the FAT32 layer. It is also the first rung where
a bug damages state that outlives the fix. A scrambled FAT stays scrambled after
the code is corrected, so the safety of the write path is not a quality-of-life
concern but the whole problem.

## Decision

Make `fs/fat32.c` writable, keeping the scope as small as the read layer's was.
Seven decisions define that scope.

**1. Whole-file writes, no handles.** `SYS_WRITEFILE(name, buf, len)` mirrors
`SYS_READFILE`: it takes the entire contents at once and the file becomes exactly
those bytes. There is no `open`, no `seek`, no descriptor table. A file handle is a
cursor into a file plus a place to hang buffering and offset state, and it wants to
be designed alongside pipes, where the same abstraction has to cover a thing with no
length. That is a later rung. Until then, a program that wants to change part of a
file reads the whole thing, edits its buffer, and writes the whole thing back.

**2. An existing file is replaced, not overwritten in place.** A write to a name
that already exists builds a *new* chain, writes the new data into it, repoints the
directory entry at it, and only then frees the old chain. It never writes over the
clusters the old file is using. This is the entire safety argument and it gets its
own section below.

**3. Delete lands in the same rung.** Not because it is cheap — it is the other
half of chain management — but because without it there is no way to prove clusters
are not leaking. A writer that only allocates can hide an allocation bug forever;
the only test that catches a stranded cluster is allocate-then-free with the free
count measured on both sides, and that needs a delete. `fat32_delete` is therefore
part of this rung, not the next one.

**4. The shell's `write` takes the rest of the line as contents.** `write NAME.TXT
the rest of this line` stores everything after the filename verbatim, spaces
included, with no trailing newline added. There is no editor and no escaping. A
typed line is far shorter than one 512-byte cluster, so from the keyboard this only
ever produces single-cluster files — which is fine, because the keyboard is not
where the multi-cluster path gets tested.

**5. Multi-cluster writing is tested by a program, not by typing.**
`user/tests/F.c` writes a 16KB file (a 32-cluster chain), reads it back, and checks
it byte-for-byte and on length, exiting 0 only on an exact match and with a distinct
non-zero status per failure mode. 16KB is chosen so `read FTEST.TXT` still works
afterwards for a human spot check, and so the chain is a real chain rather than a
lucky single cluster that broken chain logic could still get right.

**6. FSInfo is invalidated, not maintained.** The FSInfo sector caches a
free-cluster count and a next-free hint; every write makes both stale. Maintaining
them correctly is a caching problem, and a cache that is subtly wrong is worse than
no cache. So on every write and delete both fields are set to the format's defined
`0xFFFFFFFF` "unknown, recount" value, and anything that cares recomputes. Its three
signatures are verified before a byte is written into it: the sector number comes
from a boot sector that could be corrupt, and writing into the wrong sector on that
guess is how a volume is destroyed.

**7. Names that do not fit 8.3 are rejected with a message, never mangled.**
`name_to_83` already returns -1 for a name too long to express in 8.3, so this is
free at the kernel layer; the shell additionally checks the name itself and says so,
because that failure is the user's and fixable by retyping, the one case worth
distinguishing from a disk error. A real filesystem would mangle a long name into a
numbered alias (`my-notes.text` → `MY-NO~1.TEX`); doing that here would create a
file the read layer, which skips long-filename entries, could never see again.

## The write ordering, and why the directory entry is the commit point

`fat32_write_file` runs seven steps, and their order is the safety of the whole
rung. It is not a tidy-looking sequence; it is the sequence in which a crash between
any two steps leaves the disk consistent.

1. **Convert the name to 8.3.** Reject it here if it will not fit.
2. **Look the name up.** Remember whether it already exists, and if so its old start
   cluster and the disk location of its directory slot. Free nothing.
3. **Allocate a new chain** for the contents (skipped for a zero-length file). Out of
   space fails here, having changed nothing anyone can see.
4. **Write the data** into the new clusters, zero-filling the last cluster past
   `len` so no previous file's bytes leak into the slack.
5. **Build the new directory entry** pointing at the new chain.
6. **Write that entry** — reusing the old slot if the file existed, else a free one.
   **This single write is the commit point.**
7. **Free the old chain**, if there was one.

Everything before step 6 is invisible. The new clusters are allocated and filled,
but no directory entry names them, so to any reader they are simply free space that
happens to hold data; and the old file, if there was one, is still completely
intact, because nothing has touched its chain or its entry. The one write in step 6
flips the name from the old contents to the new. A directory entry is 32 bytes and
lives inside a single 512-byte block, and `disk_write` moves a whole block, so that
write lands whole or not at all — there is no torn half-entry.

So the failure analysis is short. A crash **before step 6** loses only the new,
unreferenced clusters: the old file is unharmed and the lost clusters are garbage a
reformat or a scan reclaims. A crash **after step 6** has already fully succeeded.
There is no instant at which the name resolves to a half-written file. Freeing the
old chain **last** (step 7, not step 3) is the other half of the same guarantee: the
old data has to outlive the commit, because until the commit it *is* the file.

`fat32_delete` runs the same argument in reverse, and its order is therefore the
opposite: free the data first, then unpublish the name. A crash in between leaves a
lost chain — wasted space, recoverable. The other order would leave a live directory
entry pointing at freed clusters, which is corruption: a later write could be handed
those same clusters while the entry still claims them.

## Consequences

- **A file can be created, replaced, and deleted by name.** `write` and `delete`
  work from the shell, `SYS_WRITEFILE`/`SYS_DELETE` from any program, and the disk
  is no longer a read-only build input. What the kernel writes survives a reboot,
  which is what proves it wrote a disk rather than a cache.

- **Every FAT copy is written, though only the first is read.** `fat32_set_entry`
  loops over all `num_fats` copies on every edit. Updating only the first copy is
  the single most likely bug in the rung and the nastiest, because MiniOS reads the
  first copy and so stays self-consistent while the host's tools see the copies
  disagree — the symptom is entirely off-machine. It preserves the reserved top four
  bits of each entry rather than clobbering them, for the same reason: those are not
  ours to write.

- **No file handles, no seek, no append.** A write is the whole file. There is no
  way to open a file, move to an offset, and write part of it; appending means
  reading the file, growing the buffer, and writing it all back. This is decision 1,
  and it waits on the same design work as pipes.

- **A replacement briefly needs room for both chains.** Because the old chain is
  freed only after the new one is committed (decision 2), replacing an N-cluster
  file with an M-cluster file needs N+M clusters free during the operation, not
  `max(N,M)`. On a nearly full volume a replacement can fail for lack of space that
  freeing first would have made available. That is the deliberate cost of never
  overwriting live data; freeing first would trade crash-safety for it, which is the
  wrong trade for the one rung where a bug outlives its fix.

- **No subdirectories.** A directory is a file, and the write path can grow the root
  directory (allocate a cluster, zero-fill it, link it on) when it fills, which the
  16-entries-per-cluster root reaches quickly in testing. But there is still no way
  to *create* a subdirectory: no `.`/`..` entries, no `mkdir`, and lookups remain
  root-only. Directory creation is a rung of its own.

- **No timestamps.** The directory entry's create and write date and time fields are
  left zero. MiniOS keeps no wall clock to stamp them with, so a written file shows
  as `1980-00-00 0:00` to the host's tools — the FAT epoch, which is what a zero date
  means. This is harmless (nothing on the volume reads a timestamp) and honest
  (inventing a fixed fake date would be worse).

- **FSInfo is invalidated, not maintained.** After any write or delete, the cached
  free count and next-free hint read `0xFFFFFFFF`. A host tool that trusts FSInfo
  recomputes; one that recounts anyway is unaffected. MiniOS itself never reads
  FSInfo — its own next-free hint is an in-memory global (`fs_next_free_hint`), the
  same idea one layer up.

- **The free-cluster count is observable from ring 3.** `fat32_free_count` walks the
  whole FAT and is exposed through a syscall (`SYS_FREECOUNT`) and a shell `free`
  command, so the leak test can watch the count hold steady across write/delete
  cycles. It is a full recount that trusts no cached total, which is the point: a
  cached count could hide the very leak the test exists to catch. It is deliberately
  off the allocation path (that uses the hinted `find_free_cluster`).

- **Crash safety is ordering, not journalling.** The write ordering makes a crash
  lose at worst some unreferenced clusters; it does not make the filesystem
  journalled or `fsync`-correct in the database sense. `disk_write` issues a cache
  flush ([0013](0013-ata-pio-disk-driver.md)), so a completed write is durable, but
  there is no log to replay and no torn-write protection beyond "one entry fits in
  one block". FAT32 has essentially no crash safety of its own, as
  [0014](0014-read-only-fat32.md) noted; this rung buys the one guarantee its
  structure allows and no more.

## Related

- The read-only layer this extends, and the scope it set:
  [0014](0014-read-only-fat32.md).
- The block device underneath, and its cache flush on write:
  [0013](0013-ata-pio-disk-driver.md), [../reference/disk.md](../reference/disk.md).
- The reference page for the system as it is now:
  [../reference/fat32.md](../reference/fat32.md).
- The syscalls and shell commands that expose it:
  [../reference/syscalls.md](../reference/syscalls.md),
  [../reference/shell.md](../reference/shell.md).
- The heap the cluster and directory buffers come from:
  [0010](0010-kernel-heap-ported-from-p5.md).
