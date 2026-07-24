# Chapter 17: Loading a Program

> Read chapter 16 (the filesystem) first. This chapter uses it: a program is a
> file, and you need to be able to read files by name before you can run one.

## Where we are

Your kernel runs three programs. They interleave, they have their own private
memory, they make system calls. They look like real processes.

But they are not really programs. They are **part of the kernel**.

They were written in a C file that gets compiled into the kernel image. The
linker put them in a section called `.user_text`, and the kernel copies them out
of itself into each process's memory. To change one, you recompile the whole
kernel.

That is a strange arrangement. It means the kernel and every program it can ever
run are welded into a single file. There is no way to add a program. There is no
way to remove one. The set of programs is decided at compile time, forever.

Real operating systems do not work like that, and the difference is the whole
subject of this chapter:

> **A program should be a file. The kernel should be able to run a file it has
> never seen before, without being recompiled.**

That is what a loader does.

## The problem: bytes do not explain themselves

Say you read `hello.bin` off the disk. You now have a pile of bytes sitting in
memory. Two thousand of them, say.

Now what?

You need to answer three questions, and the bytes do not answer any of them:

1. **Which bytes are instructions?** Not all of them are. A program has code, but
   it also has constants, string literals, lookup tables. Nothing about a number
   tells you whether it is an instruction or data. `0x48` could be part of an
   instruction, or the letter H, or the number 72.
2. **Where in the fake street do they go?** The program was written expecting its
   code to live at a particular address. Put it somewhere else and every jump and
   every reference inside it points at the wrong place.
3. **Which byte does the finger start on?** Not necessarily the first one. The
   entry point could be anywhere in the pile.

There is no way to work this out by inspection. You cannot look at bytes and
deduce their meaning. The information simply is not in them.

## The answer: a manifest

So the file carries the answers with it, in a **manifest** at the front. A cover
sheet, written by the compiler and linker, that says something like:

```
I am an executable, for 64-bit x86 machines.
Start the finger at address 0x401000.

Take bytes 0x1000 to 0x2500 of this file, put them at address 0x400000.
Take bytes 0x2500 to 0x2600 of this file, put them at address 0x600000,
    and after copying, make that region 4096 bytes long by zeroing the rest.
```

Now the pile of bytes explains itself. The loader does not need to be clever. It
does not need to understand the program at all. It reads the manifest and follows
the instructions like a recipe.

**That is the entire job of a loader.** Read the cover sheet, copy the chunks
where the cover sheet says, jump to the address the cover sheet names. It is a
much dumber job than the word "loader" suggests.

The manifest format is called **ELF** (Executable and Linkable Format). Your
kernel is an ELF file. Every program on Linux is an ELF file. It is not exotic.
It is a header, followed by a list of "put these bytes there" entries.

## The two pieces of ELF you actually need

ELF is a large format with a lot in it. A loader needs a small corner of it.

### The ELF header

The first bytes of the file. What you care about:

- **A magic marker.** The first four bytes are `0x7F` followed by `E`, `L`, `F`.
  If they are not, this is not an ELF file and you stop.
- **Class and machine.** Is this a 64-bit file? Is it for x86-64? A 32-bit ARM
  binary is a perfectly valid ELF file and would be nonsense to run here.
- **Type.** Is this an executable, or something else (an object file, a library)?
- **The entry point.** The address where the finger starts.
- **Where the program headers are**, how many there are, and how big each one is.

### The program headers

A list of entries, each describing one chunk to load. Each says:

- **type**: is this chunk something to load into memory, or metadata you can
  ignore? Most entries are the loadable kind, some are not.
- **offset in the file**: where the bytes are in the file
- **virtual address**: where they go in memory
- **size in the file**: how many bytes to copy
- **size in memory**: how big the region should end up (this is not a typo, see
  below)
- **flags**: readable, writable, executable

That is it. That is everything a loader reads. The rest of ELF exists for linkers
and debuggers, and a loader can walk straight past it.

## The load loop

The whole loader, in shape:

```
read the file into memory
check the magic, the class, the machine, the type   (bail if wrong)
for each program header:
    if it is not a loadable chunk, skip it
    copy `size in the file` bytes from `offset in the file`
                                    to `virtual address`
    zero the bytes from (virtual address + size in file)
                     to (virtual address + size in memory)
set the finger to the entry point
```

Eight lines. That is a program loader.

## The size-in-file versus size-in-memory thing

That is the one line that looks like a mistake, so it deserves its own section,
because getting it wrong produces a bug that looks completely random.

**A chunk can be smaller in the file than it is in memory.** On purpose.

Why? Uninitialized globals.

```c
int counter;          // starts at zero
char buffer[4096];    // starts at all zeros
```

Those need 4100 bytes of memory, all of it zero. But storing 4100 zeros in the
file would be silly. The file would grow by 4KB to carry no information at all.
A program with a one megabyte buffer would be a one megabyte file that is
entirely nothing.

So the file does not store them. The manifest just says: **"this chunk is 12 bytes
in the file but 4112 bytes in memory."** The loader copies the 12 real bytes and
then **zeroes the remaining 4100 itself**.

That zeroed region has a traditional name (BSS) that tells you nothing, so ignore
the name. What matters is the rule:

> Copy `size in file` bytes. Then zero everything from there up to
> `size in memory`. The gap is uninitialized data, and it is the loader's job to
> make it zero.

**If you skip the zeroing**, your program's globals come up holding whatever
garbage was in that memory before. Sometimes that is zero by luck and everything
works. Sometimes it is not, and your program behaves differently depending on what
ran before it. That is a genuinely awful bug to chase, and it is one of the two
classic loader mistakes.

(The other classic mistake is loading at the wrong address, which fails
immediately and loudly, so it is much kinder.)

## The loader must not trust the file

This is worth being careful about, because it is a real hole and it is easy to
miss.

The manifest is instructions from a file. **The file is not trustworthy.** It came
off a disk. Anyone could have put it there. And the instructions it contains are
literally of the form "write these bytes to this address."

So a naive loader that just does what the manifest says is a loader that will
happily write attacker-chosen bytes to an attacker-chosen address. Point a
manifest at the kernel's memory and you have handed over the machine, using
nothing but a file.

Two checks are mandatory:

1. **Validate the header before believing anything.** Magic, class, machine, type.
   A file that fails any of them is rejected, not interpreted. Never parse
   further into a structure you have not confirmed is that kind of structure.
2. **Bounds-check every address.** For every chunk, check that the whole range
   (`virtual address` through `virtual address + size in memory`) sits inside the
   region the program is allowed to occupy. Anything outside, reject the file.

This is the same principle as the system call pointer check from chapter 11: the
kernel has privileges the caller does not, so when the caller chooses an address,
the kernel must check it. Here the "caller" is a file, which is even less
trustworthy than a program.

**Rule of thumb: any time untrusted input names an address, bound it.**

## And then the payoff

Here is the nice part.

Once you have followed the manifest, what do you have? An address space with the
right bytes at the right addresses, and an entry point.

**That is exactly what `task_create` already builds.** It builds a private address
space, puts the program's bytes in it, and forges a frame whose finger points at
the start.

The only difference is where the bytes came from. Today they come from a symbol
the compiler baked into the kernel. After this, they come from a file.

> **Loading a program is `task_create` with a different source for the bytes.**

Everything downstream is untouched. The forged frame, the private page tree, the
scheduler, the CR3 switch, the syscall path. None of it changes or even notices.
That is what a clean layer boundary looks like: you swap out the bottom and
nothing above it moves.

## What has to change on the build side

One practical consequence worth understanding, because it is more work than the
loader itself.

Right now the user programs are compiled *into* the kernel. After this they have
to be compiled *separately*, into their own files, which then get copied onto the
disk image.

That means each user program needs:

- its own build rule, producing its own binary
- its own linker script, saying "link this to run at address 0x400000"
- to be freestanding and static, exactly like the kernel: no C library, no
  startup code, nothing from the host system
- to be copied onto the disk image as part of the build

The user program can still use the syscall header, because that header was
deliberately written to be standalone (numbers only, no kernel types, no
includes). That was not an accident. It was written that way so a program
compiled entirely separately could still speak to the kernel.

## What this still is not

Being precise about what has been built, because "program loading" covers more in
a real system:

- **No arguments.** Real programs get `argv` and an environment. Here a program
  just starts. Passing arguments means setting up a specific layout on the new
  program's stack before jumping in, which is its own piece of work.
- **No dynamic linking.** Real programs often reference code that lives in shared
  libraries, and something has to find those libraries and patch the addresses at
  load time. This loader handles only fully static programs where everything is
  in the file.
- **No relocation.** This loader puts each chunk at the exact address the manifest
  names. A relocatable program can be loaded anywhere, but only if the loader
  then walks a table of "fix up this reference" entries. That is what makes
  address randomization possible, and it is a separate mechanism.
- **No demand paging.** The whole program is read off the disk and into memory
  before it runs. Real systems map the file and pull pages in as the program
  actually touches them, so starting a large program is fast and unused parts are
  never read at all.

Each of those is a real feature and none of them are needed to run a program.

## Exercises

1. You have 2000 bytes read off the disk. Explain why you cannot determine, by
   examining them alone, which are instructions.
2. A program header says: file size 40, memory size 8192. Explain what the file
   contains, what the loader must do, and what the program would experience if
   the loader ignored the difference.
3. A malicious file's manifest says "copy 512 bytes to address 0x100000" (where
   the kernel lives). Describe exactly what happens with a naive loader, and the
   single check that prevents it.
4. Why does the loader validate the ELF magic *before* reading the entry point,
   rather than after?
5. The entry point is stored in the manifest rather than being assumed to be the
   first byte of the file. Give a reason a compiler might not put the starting
   instruction first.
6. `task_create` currently copies from a compile-time symbol. State precisely
   which parts of it must change to load from a file, and which parts do not
   change at all.
7. Two processes run the same program file. With this loader, how many physical
   copies of the program's code exist in memory? What would a real operating
   system do instead, and what would it have to be careful about?
