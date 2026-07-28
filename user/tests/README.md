# Kernel test fixtures

These are ring-3 programs that exist to prove a piece of the kernel works. They are
not part of the machine's runtime: nobody would want them on a computer they were
using. The shell (`../shell.c`) is the real program, and `../userlib.h` and
`../user.ld` are the runtime every program here compiles against.

They build exactly like any other user program — freestanding, static,
`-mcmodel=small`, linked at `0x400000` — and they land in the **root directory** of
the disk image alongside `SHELL.ELF`, because `fs/fat32.c` can only look up a bare
8.3 name in the root. The directory is a source-tree convention; the disk has no
directories. Run one from the shell with `run a.elf` (the lookup is
case-insensitive).

Each program is deliberately strange in some way. **Read the paragraph before you
change one**: what looks like an oversight is usually the thing being tested.

## A.ELF — the ordinary case

Prints `A` twenty times with a delay between each, then exits with status 0. It has
a zero-initialised global so the binary has a real `.bss`, which keeps the ELF
loader's zero-fill honest: a program with no `.bss` would load correctly even if
that step were missing.

    > run a.elf
    run: started a.elf
    AAAAAAAAAAAAAAAAAAAA
    reap: task 1 exited (status 0), free frames: 30592
    run: a.elf exited with status 0

This is the baseline for the memory test. Run it ten times and the free frame count
must be identical from the first onwards.

## B.ELF — a second, longer program

Identical in shape to A but sixty rounds instead of twenty, so the two are visibly
different lengths of work. It exists so the scheduler has more than one file to
interleave and the loader is proven on more than one binary. Exits with status 0.

## C.ELF — a non-zero exit status

Prints `C` forty times, then exits with **status 3**. Everything else in the system
exits 0, which is also what an uninitialised field and a dropped value look like.
Three is a number nothing else produces, so `run: c.elf exited with status 3` at the
prompt proves the value survived the whole trip: out of the program's `sys_exit`,
through the kernel's mask, into the zombie's `exit_status`, back through the
parent's `SYS_WAIT`, and into a number printer in ring 3.

## Why every loop is bounded

There is no way to kill a task and there are no signals. A program that never exits
leaves its parent blocked in `SYS_WAIT` with no way back short of a reboot, so
`run <that program>` would make the shell permanently unusable. Every program here
therefore runs a fixed number of rounds and calls `sys_exit` at the bottom. There is
no crt0 either, so falling off the end of `_start` is undefined behaviour rather than
an implicit `exit(0)`: the call has to be written out.
