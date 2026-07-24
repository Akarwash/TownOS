# ELF program loading

This is the factual description of MiniOS's program loader, read from
`kernel/elf.c`, `kernel/elf.h`, `user/user.ld`, and the `USER_*` rules in the
`Makefile`. It loads statically linked ELF64 executables off the FAT32 disk image
and runs them as ring-3 tasks. For why programs became files at all, and what the
loader deliberately does not do, see
[decisions/0015](../decisions/0015-elf-program-loading.md).

## The manifest idea

A pile of bytes read off a disk does not explain itself. Nothing about them says
which are instructions and which are constants, what address they expect to live
at, or which byte to execute first. An executable format is what carries those
answers alongside the bytes.

ELF splits a file into two parts for exactly this reason:

- **A manifest** at the front: a header saying what kind of file this is and
  where execution starts, then a table of program headers, each of which is one
  instruction to the loader of the form *copy N bytes from file offset O to
  address V, make the region M bytes long, with these permissions*.
- **The payload**: the bytes those entries point at.

Loading is therefore mechanical. Read the manifest, do what it says. The
interesting parts are all in what you check before you do what it says.

Section headers are the other half of ELF and are ignored here completely. They
describe the file for linkers and debuggers (which bytes are `.text`, where the
symbol table is); a loader needs none of it.

## The two structures the loader reads

### The ELF header, at offset 0

| Offset | Field | Used for |
|--------|-------|----------|
| 0 | `e_ident[16]` | Magic `0x7F 'E' 'L' 'F'`, then class, endianness, version |
| 16 | `e_type` | Must be `ET_EXEC` (2), a fixed-address executable |
| 18 | `e_machine` | Must be `EM_X86_64` (62) |
| 24 | `e_entry` | Virtual address of the first instruction |
| 32 | `e_phoff` | File offset of the program header table |
| 54 | `e_phentsize` | Size of one program header, must be 56 |
| 56 | `e_phnum` | How many program headers |

`e_ident` is the same 16 bytes in ELF32 and ELF64, which is the point of it: a
reader can work out which of the two it is holding before committing to a
structure layout.

`e_entry` is where the entry point comes from. Nothing about it is known at
kernel link time, which is precisely what makes the program a file rather than
part of the kernel.

### The program header, 56 bytes each

| Offset | Field | Used for |
|--------|-------|----------|
| 0 | `p_type` | `PT_LOAD` (1) means load it; anything else is skipped |
| 4 | `p_flags` | `PF_R` (4), `PF_W` (2), `PF_X` (1) |
| 8 | `p_offset` | Where the bytes are in the file |
| 16 | `p_vaddr` | Where they go in memory |
| 32 | `p_filesz` | How many bytes are **in the file** |
| 40 | `p_memsz` | How many bytes the segment **occupies in memory** |

Both structs are `__attribute__((packed))`, the same trap as the Multiboot mmap
entry and the FAT32 BPB. Their fields happen to be naturally aligned today, so
the attribute changes nothing right now, which is exactly why it is easy to omit
and why `kernel/elf.c` also carries compile-time size guards (64 and 56 bytes).
The moment a field moved or a smaller one were added, padding would shift every
field after it and the parse would silently read the wrong bytes.

## Why file size and memory size differ

This is the field pair that matters most, and the one whose mishandling produces
the most confusing bug in the whole loader.

A program's zero-initialised globals (`.bss`) occupy memory but are not stored in
the file. Storing them would mean writing thousands of zero bytes to disk to say
"these are zeros". So the format says it once instead: the segment occupies
`p_memsz` bytes, of which the first `p_filesz` are in the file, and **everything
past that must be zero**.

The loader therefore has to zero the gap itself. `alloc_frame` hands back a frame
holding whatever the previous user of that memory left in it. Skip the zero-fill
and a program's globals come up holding that garbage, so the program works or
fails depending on what ran before it, which is about the worst failure mode a
bug can have.

`kernel/elf.c` zeroes the whole frame and then copies the file bytes over the
front of it, rather than copying first and zeroing the tail. It writes some bytes
twice and makes the boundary case impossible to get wrong.

MiniOS's own programs make this concrete. Each carries a zero-initialised global
specifically so it has a real `.bss`, and `readelf -l A.ELF` shows the result:

```
LOAD  offset 0x1000  vaddr 0x400000  filesz 0x2000  memsz 0x2000   R E
LOAD  offset 0x3000  vaddr 0x402000  filesz 0x0     memsz 0x1000   RW
```

The second segment stores nothing at all and occupies a full page. Without the
zero-fill it would come up as a page of stale garbage. A program with no `.bss`
would load correctly even with the zero-fill missing, which is why the global is
there on purpose.

## Validation: what is checked, and why it is not optional

The manifest arrives from a file on a disk. The kernel did not write it and
cannot vouch for it. So the loader validates first and parses second, and never
reads a field out of a structure it has not already confirmed is that kind of
structure.

In order:

1. The file is at least as long as an ELF header, before the header is read as
   one.
2. The magic, before the class.
3. The class (`ELFCLASS64`), endianness (`ELFDATA2LSB`) and version, before
   anything 64-bit-shaped is dereferenced.
4. `e_machine` is x86-64 and `e_type` is `ET_EXEC`. `ET_DYN` is rejected rather
   than attempted: a shared object needs its relocations applied and this loader
   does not relocate.
5. `e_phentsize` matches the loader's idea of a program header. If it does not,
   the stride through the table is wrong and every entry after the first is read
   from the wrong offset.
6. `e_phnum` is non-zero and not implausible (a cap of 32; it sizes a loop over
   untrusted data).
7. The program header table lies entirely inside the file, before a single entry
   is read out of it.
8. Per `PT_LOAD` segment: `[p_offset, p_offset + p_filesz)` lies inside the file,
   `p_memsz >= p_filesz`, and `p_vaddr` and `p_memsz` are page-aligned.
9. At least one loadable segment exists.

Every failure prints a specific reason naming the file, and returns -1. A
malformed file is rejected, never interpreted on the assumption the rest of it is
probably fine.

One detail worth copying: the in-file range check is written as two subtractions
against the known-good file size rather than the obvious `offset + length <=
size`. Both values come from the file and are 64-bit, so their sum can wrap, and
a wrapped sum compares as comfortably small.

## The bounds check: the security boundary

Every `PT_LOAD` entry is, literally, an instruction from an untrusted file that
reads *write these bytes to this address*. Without a bound on the address, a
crafted file names any address it likes, and the kernel obligingly maps a page
there and copies attacker-chosen bytes into it. Over the kernel's own code, for
instance. A merely corrupt `p_vaddr` does the same damage without any intent.

So before a single frame is allocated, the whole destination range is checked:

```
ELF_LOAD_MIN_VADDR = USER_REGION_START   (0x400000, kernel/memory.h)
ELF_LOAD_MAX_VADDR = USER_STACK_BASE     (0x7C0000, kernel/usermode.h)

reject unless p_vaddr >= MIN and p_vaddr + p_memsz <= MAX
```

The upper bound stops at the bottom of the fixed user stack rather than at the
top of the user region, because the stack is mapped separately at a known
address; a segment reaching into it would have the two fighting over the same
pages.

This is the same category of check as the `SYS_WRITE` pointer validation in
`kernel/syscall.c` (see [syscalls.md](syscalls.md)), and it is the stricter of
the two: this one bounds the entire `[start, end)` range, while the syscall check
still only tests the start pointer.

## The load loop

For each `PT_LOAD` segment, in `load_segment`:

1. **Bounds-check** the destination range, as above. Reject the file otherwise.
2. **Derive the page flags** from `p_flags`: always `PG_PRESENT | PG_USER`, plus
   `PG_WRITABLE` only if `PF_W` is set. Only the write bit is actually
   enforceable, since MiniOS does not enable NX, so R+X and R map identically.
   Leaving the writable bit off for text is real, though: a program cannot
   overwrite its own code.
3. **For each page** of `p_memsz`: allocate a frame, zero it, copy in whatever
   part of the file covers that page, and map it into the target address space at
   `p_vaddr + offset` with those flags.

The copy writes through the frame's identity-mapped physical address, not through
the new user mapping, which is why a page mapped read-only into the task can
still be filled.

## The read path around it

`elf_load_file(name, as, &entry)` is the whole sequence for a program on disk:

1. `fat32_stat(name, &size)`. The size has to be known before a buffer can be
   allocated for it. The alternative, a fixed buffer assumed to be big enough, is
   a limit that fails quietly the day a program outgrows it.
2. `kmalloc(size)`, then `fat32_read_file` into it. The whole file is read before
   anything runs; there is no demand paging.
3. `elf_load` on the buffer.
4. `kfree` the buffer. Its contents are now in the task's own frames, so nothing
   needs the copy afterward.

`task_create_from_file` (`kernel/scheduler.c`) wraps that: create a fresh address
space, load the program into it, map a stack at the fixed stack VA, and forge the
task's register pile with the entry point from the ELF header. It is deliberately
the same shape as the old compiled-in path, and the page tree, the stack mapping,
the forge, `schedule()` and the CR3 switch are all unchanged. The only things
that differ are where the bytes come from and where the entry address comes from.

## The separate user build

A user program is compiled and linked entirely on its own. Nothing about it is
part of `minios.bin`.

```
CC -ffreestanding -m64 -mno-red-zone -mcmodel=small -fno-pie -no-pie \
   -nostdlib -nodefaultlibs -static -Wall -Wextra \
   -T user/user.ld -Wl,-z,max-page-size=4096 -Wl,--build-id=none \
   -o user/A.ELF user/A.c
```

- **`-mcmodel=small`, not `-mcmodel=kernel`.** The kernel model assumes every
  symbol lives in the top 2GB of the address space. User code links at 0x400000,
  nowhere near it, and the kernel model produces relocation errors. This is the
  same trap the original user-mode work hit.
- **`-static -nostdlib -nodefaultlibs`.** No host libc, no startup files. The
  entire runtime a program gets is `user/userlib.h`: inline-asm syscall wrappers
  (still `always_inline`) over the standalone `include/syscalls.h` and
  `include/vectors.h`.
- **`-fno-pie -no-pie`.** Position dependent. The loader does not relocate, so a
  program must be linked at exactly the address it will be loaded at.
- **`-z max-page-size=4096`.** Without it the linker aligns segments to its
  default 2MB, padding each binary enormously for no benefit.

`user/user.ld` links at 0x400000, names `_start` as the entry, and declares two
load segments (R+X for text and rodata, R+W for data and bss). It starts every
loadable segment on a 4096-byte boundary **and** rounds its size up to a whole
number of pages, using a trailing `. = ALIGN(4096)` inside each output section
(which is what rounds the size, not just the start).

That alignment is a deliberate contract with the loader, which requires it and
rejects a file that violates it. The loader maps whole pages, so a segment
starting or ending mid-page would put two segments in one page, and the second
mapping would have to merge with the first rather than replace it, with the
stricter permissions winning. Since the linker script is entirely under our
control, satisfying the constraint there costs one line per section and saves
that work in the kernel.

## Getting a program onto the disk

The binaries are copied onto the FAT32 image with `mcopy`, as a PHONY make target
that `make run` depends on:

```bash
mcopy -o -i disk.img user/A.ELF user/B.ELF user/C.ELF ::/
```

PHONY on purpose. The image is created once and then left alone (reformatting
would destroy its contents), but the program binaries on it are build output and
must never be stale. Running yesterday's `A.ELF` looks exactly like a loader bug.

Names are 8.3 and uppercase because the filesystem reads 8.3 names only, and the
source files are named to match (`user/A.c` builds `A.ELF`) so the mapping needs
no explaining.

## What the loader does not do

- **No arguments, no argv.** A program is entered with a forged frame and an
  empty stack, and there is no channel to pass it anything.
- **No dynamic linking, no relocation.** `ET_EXEC` at a fixed address only.
- **No demand paging.** The whole file is read and every segment fully populated
  before the first instruction runs.
- **No sharing.** Each task gets its own physical copy of the program's text
  (`TODO(shared-text)`).
- **No NX.** Only the write bit of `p_flags` reaches the page tables.

## Where to read more

- The decision and its consequences:
  [decisions/0015](../decisions/0015-elf-program-loading.md)
- The filesystem the file is read through: [fat32.md](fat32.md)
- The address space segments are mapped into: [paging.md](paging.md)
- The task that runs the result: [scheduling.md](scheduling.md)
- The concepts behind all of this:
  [`../../learnings/17-loading-a-program.md`](../../learnings/17-loading-a-program.md)
