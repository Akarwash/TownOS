# 0021 - Expose fat32_stat to ring 3 as SYS_STAT, and stat before reading

## Status

Accepted.

## Context

`fat32_stat(name, &size)` has existed since the read-only FAT32 rung
([0014](0014-read-only-fat32.md)). It reports a file's size straight from its
directory entry, touching no file data, and it was written for exactly the caller
that could not otherwise exist: reading a file means allocating a buffer for it
first, which means knowing the size first, and `fat32_read_file` cannot answer
that because it needs the buffer up front. The function was there; it had never
been reachable from ring 3.

So the shell did the thing a caller reaches for when it cannot ask a size: a fixed
buffer assumed to be big enough. `read` reads through `SYS_READFILE` into a 32KB
`SHELL_FILE_MAX` buffer, asking for at most 32767 bytes (one held back for the NUL
it appends). That is a size limit waiting to be exceeded quietly, and the writable
rung's `HUGE.TXT` (40981 bytes, [0020](0020-writable-fat32.md)) is the file that
exceeds it.

Two problems surfaced there, both from the same missing size.

**`read` could not tell why it failed.** `fat32_read_file` refuses a file larger
than the buffer outright — it compares the directory size against `bufsize` and
returns -1 before reading a cluster — so `SYS_READFILE` fails and `read` printed
`read: cannot read huge.txt`. But that one line was three different failures
wearing one message: the file is absent, the file is too big, or the disk errored.
A user got the same words for all three and no way to tell which had happened.

**The truncation notice was unreachable dead code.** `cmd_read` carried a notice —
"showing the first 32767 bytes, the file may be longer" — meant for a
`SYS_READFILE` that fills the buffer and stops. It does not; it delivers the whole
file or refuses it, never a prefix. So the notice's condition (bytes read equals
the buffer size) could only be true for a file of *exactly* 32767 bytes, which is
complete — the single case that reached the notice was the one case where it was
wrong. This was tracked as `TODO(read-truncation)`.

Both problems dissolve if the shell asks the size before it reads, which is what
`fat32_stat` was written to answer. It just needed a door to ring 3.

## Decision

**Add `SYS_STAT` (10), a thin syscall over `fat32_stat`.**

```
#define SYS_STAT 10   // RDI = name, RSI = uint64_t *out_size; 0 on success, SYSCALL_ERROR if not found
```

It reports a file's size from its directory entry without reading its contents,
and returns `SYSCALL_ERROR` if the file is not found — folding "name is not 8.3"
and "names a directory" into the same not-found error, because from the shell's
point of view all three mean "there is no file of that name to read".

Both pointers come from ring 3 and are untrusted. The filename is copied in and
length-capped exactly like `SYS_RUN`'s. The `out_size` pointer is a **write
target**, not just a read: the kernel writes eight bytes through it, so the whole
`[ptr, ptr+8)` range is bounds-checked with `user_range_ok`, the same check
`SYS_READFILE` applies to its destination buffer. Validating only the start
pointer — as the `SYS_WRITE` stopgap does — would let a pointer sitting just below
`USER_REGION_END` have the kernel write a `uint64_t` off the end of the region and
into kernel pages.

`SYS_STAT` **does not block**, so unlike `SYS_READKEY` and `SYS_WAIT` it has no
RAX-discipline problem: it computes an answer and returns it through the dispatcher
normally, and `rax` is never left holding the call number across a reschedule.

**The shell stats first, then decides.** `cmd_read` becomes:

- not found → `read: no such file: X`
- found but larger than the buffer → `read: X is N bytes, the buffer holds M`
- otherwise → read and print as before

The unreachable truncation notice and the `TODO(read-truncation)` marker are
removed, from the shell and from the reference page that recorded them.

## Consequences

- **The three failures now read apart.** Absence, too-big, and a genuine disk
  error are three different lines. The size is what tells the first two apart from
  each other; only a real read error falls through to the old `read: cannot read
  X`, which now means what it says.

- **No partial reads.** `read` on a large file still refuses; it just now says why,
  with numbers. It delivers the whole file or none of it, never a prefix a caller
  could mistake for the whole thing. This is a deliberate non-change: adding
  truncation was the other way to make the old notice honest, and it was rejected
  because it is the larger, contract-breaking change of the two.

- **No offset.** Showing part of a large file would need an offset argument on
  `SYS_READFILE` — read bytes `[off, off+len)` of a file rather than the whole
  thing — which changes `SYS_READFILE`'s contract and every caller of it. That is a
  rung of its own and is deliberately not taken here. `SYS_STAT` is the smaller
  half: it lets the shell *decline with a reason*, not read a piece.

- **`stat` returns a size and nothing else — not a type, not a timestamp.**
  `fat32_stat` already refuses a directory, so `SYS_STAT` answers only "how big is
  this file" and folds "not a file" into the not-found error. A caller that wanted
  to tell a directory from a missing name, or wanted a modification time, would
  need more of the directory entry surfaced — and MiniOS keeps no clock, so the
  entry's time fields are zero anyway ([0020](0020-writable-fat32.md)). A richer
  stat is future work if a caller ever needs it; today the one caller needs a size.

- **The stat and the read are not one atomic operation, and do not need to be.**
  A file could in principle change between the size check and the read. The read
  re-checks the size against the buffer itself (`fat32_read_file` refuses `size >
  bufsize`), so a file that grew in between fails the read safely rather than
  overrunning the buffer — the stat is an optimisation of the error message, not a
  load-bearing guarantee.

- **Eleven syscalls now.** The dispatcher gains one more numbered case; the
  `int 0x50` gate, the calling convention, and the pointer-check helpers are
  unchanged.

## Related

- The syscall gate and calling convention:
  [0007](0007-syscalls-via-int-0x50.md),
  [../reference/syscalls.md](../reference/syscalls.md).
- The `fat32_stat` this exposes and the read path it sizes:
  [0014](0014-read-only-fat32.md), [../reference/fat32.md](../reference/fat32.md).
- The shell that now stats before it reads:
  [0016](0016-interactive-shell.md), [../reference/shell.md](../reference/shell.md).
- The file that outgrew the buffer and made the missing size visible:
  [0020](0020-writable-fat32.md) and the `HUGE.TXT` entry in
  [../../CHANGELOG.md](../../CHANGELOG.md).
