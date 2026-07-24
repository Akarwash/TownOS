# 0015 - Load programs from disk as ELF64 binaries

## Status

Accepted.

## Context

MiniOS's ring-3 programs were part of the kernel. They were written in
`user/user_program.c`, compiled into the kernel image, placed by `linker.ld` in a
`.user_text` section at 4MB inside `minios.bin`, and copied out of the kernel
into each task's private frames by `task_create`. The kernel and every program it
could ever run were a single file.

That has consequences beyond the obvious inconvenience. Changing a program meant
recompiling the kernel. Adding one meant editing the kernel's source, its linker
script, and `kernel_main`. Removing one meant the same. The set of programs was
decided at kernel link time, permanently, and `task_create` took a compile-time
symbol address as its entry point, so the kernel could only start something it
already knew the name of.

[0014](0014-read-only-fat32.md) removed the reason for that arrangement. There is
now a filesystem: files on the disk can be found by name and read into memory.
Reading is the complete prerequisite for loading a program, because a program
image is read and never written, which is exactly why the filesystem was built
read-only first.

What was still missing is the step between "a pile of bytes from a file" and "a
running program". Bytes do not explain themselves: nothing about them says which
are instructions, where they expect to live in memory, or which one to execute
first. Something has to carry that information alongside the bytes, and that is
what an executable format is.

## Decision

User programs become separately compiled, statically linked ELF64 binaries that
live on the FAT32 disk image, loaded at runtime by an in-kernel loader
(`kernel/elf.c`).

**The build side.** Each program is built on its own from `user/A.c`, `user/B.c`,
`user/C.c` against `user/user.ld`, with `-ffreestanding -nostdlib -static
-fno-pie -no-pie -mcmodel=small`. `-mcmodel=small` specifically, not the kernel's
`-mcmodel=kernel`, which assumes every symbol lives in the top 2GB of the address
space and produces relocation errors on code linked at 4MB. The whole runtime a
program gets is `user/userlib.h`: inline-asm syscall wrappers over the standalone
`include/syscalls.h` and `include/vectors.h`. The binaries are copied onto the
image with `mcopy`, and `make run` re-copies them on every boot so a stale
program can never be what runs.

**The loader.** `kernel/elf.c` parses the ELF header and the program headers, and
nothing else. Section headers describe the file for linkers and debuggers and say
nothing a loader needs. For each `PT_LOAD` segment it bounds-checks the
destination, allocates frames, maps them into the target address space with flags
derived from the segment's own, copies the file bytes, and zeroes from the
segment's file size up to its memory size.

**Segments are page-aligned by contract.** `user/user.ld` starts every loadable
segment on a 4096-byte boundary and rounds its size up to a whole number of
pages; the loader requires this and rejects a file that violates it. The
alternative is handling two segments that share a page, where the second mapping
must merge with the first rather than replace it, with the stricter permissions
winning. Since we control the linker script completely, satisfying the constraint
there is far cheaper than supporting the general case in the kernel.

**Validation before interpretation.** The loader validates identification, class,
endianness, version, machine and type before reading anything else, then checks
the program header table's own bounds before reading a single entry out of it,
then checks each segment against the file's bounds, `memsz >= filesz`, and the
alignment contract. A file failing any check is rejected with a named reason.

**The bounds check is the security boundary.** Every `PT_LOAD` entry is an
instruction from an untrusted file of the form "write these bytes to this
address". The destination range is checked against the window a user program may
occupy before a single frame is allocated.

## Consequences

- **A program is a file.** Changing what the machine runs means rebuilding one
  small binary and copying it onto the image. Demonstrated by editing `user/A.c`
  to print `Z`, rebuilding only `A.ELF`, and booting a byte-identical
  `minios.bin` (same MD5 as before the edit): the screen showed `ZBCZBC`. No
  amount of passing self-tests substitutes for that demonstration, because it is
  the only one that proves the coupling is actually gone.

- **The kernel no longer contains any ring-3 code.** `user/user_program.c`, the
  `.user_text` and `.user_rodata` sections, the `:user` PT_LOAD segment, the
  `_user_text_start` family of symbols, and `task_create`'s compile-time-entry
  path are all deleted. `minios.bin` went from 45476 to 42608 bytes and from two
  PT_LOAD segments to one.

- **The loader is a security boundary, and a new one.** Before this, everything
  the kernel ran came from its own image and was trusted by construction. Now it
  acts on a manifest from a file, so `kernel/elf.c` is code where a missing check
  is an arbitrary-write primitive rather than a bug. This is the second untrusted
  input in the kernel, after the ring-3 syscall pointer, and it is bounds-checked
  more carefully than that one is (the whole `[start, end)` range, not just the
  start).

- **A bad file costs only its own task.** Each load can fail independently, and
  `kernel_main` reports the reason, skips that program, and runs the rest. Booted
  with a 520-byte text file named `BAD.ELF` and a nonexistent `MISSING.ELF` in
  the list, the kernel printed `not an ELF file (bad magic)` and `not found on
  the disk` and still ran the three real programs.

- **No arguments and no argv.** A program is entered with a forged frame and an
  empty stack; there is no way to pass it anything. Adding argv means agreeing a
  layout for it on the new stack and a convention for finding it.

- **No dynamic linking and no relocation.** Programs are `ET_EXEC`, linked at a
  fixed 0x400000, and the loader resolves nothing at runtime. A program runs at
  the address it was linked at or not at all, so two programs cannot be loaded at
  different addresses and nothing can be shared as a library.

- **No demand paging.** The whole file is read into a heap buffer, and every
  segment is mapped and populated before the program's first instruction runs.
  A large program pays its entire read cost up front, through the polled disk
  driver, which freezes the machine for the duration.

- **Each process still gets its own physical copy of the code.** The loader
  allocates fresh frames per task, so three tasks running the same program would
  hold three copies of its text. Sharing the read-only text by reference is the
  same `TODO(shared-text)` that per-process paging left behind, and the loader
  inherits it rather than fixing it.

- **Only the write permission is real.** MiniOS does not enable NX, so a segment
  marked R+X and one marked R map identically. Leaving the writable bit off for
  text is enforced though, so a program cannot overwrite its own code, which is
  more than the old copy-everything-writable path did.

- **Programs must be named in 8.3 form.** They are read through the FAT32 layer,
  which handles 8.3 names only. Hence `A.ELF` rather than anything descriptive.

## Related

- The filesystem this reads through: [0014](0014-read-only-fat32.md),
  [../reference/fat32.md](../reference/fat32.md).
- The address spaces segments are mapped into:
  [0012](0012-per-process-paging.md), [../reference/paging.md](../reference/paging.md).
- The task forge this reuses unchanged:
  [0008](0008-round-robin-preemptive-scheduler.md),
  [0011](0011-dynamic-tasks-and-stacks.md).
- The other untrusted input, and the check this one is modelled on:
  [0007](0007-syscalls-via-int-0x50.md), [../reference/syscalls.md](../reference/syscalls.md).
- Reference page: [../reference/elf-loading.md](../reference/elf-loading.md).
