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
    reap (wait):    task 1 exited (status 0), free frames: 30592
    run: a.elf exited with status 0

This is the baseline for the memory test. Run it ten times and the free frame count
must be identical from the first onwards.

`reap (wait):` names the code that freed the task's memory — here the shell's own
`SYS_WAIT`, which is what reaps a child in every ordinary case. The other label,
`reap (sweeper):`, is what D and E exist to produce.

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

## D.ELF — a parent that does not wait

Starts `E.ELF` and then exits immediately, **without calling `sys_wait`**. That
omission is the entire program; it is not a bug and must not be tidied away.

D exists to orphan E. Normally a child is reaped by its parent's `SYS_WAIT` before
the scheduler's sweeper ever sees it — the exiting child wakes its parent and
switches straight to it, so the parent re-enters `SYS_WAIT` before a timer tick has
gone by. That leaves two pieces of `reap_sweep` unreached in every other test
here: the free path that tears a zombie's address space down, and the branch of
`parent_alive` that answers "no" and drops an unwatched tombstone. `run d.elf` is
what reaches them, and the output below shows how.

    > run d.elf
    D: starting E
    run: started d.elf
    D: not waiting, exiting
    reap (sweeper): task 1 exited (status 0), free frames: 30519
    run: d.elf exited with status 0
    > EEEEEEEEEEEEEEE
    reap (sweeper): task 2 exited (status 7), free frames: 30591

Both D and E are reaped by the **sweeper**, and which path frees which is decided
by where they sit in the task table, not by luck. The table is `shell` = 0, `D` =
1, `E` = 2. When D exits it wakes the shell, its parent, but `find_next_ready` scans
forward from the slot *after* D and so reaches E at slot 2 before it wraps back to
the shell at slot 0: E, not the shell, is what runs next, and it gets a full
timeslice. The next timer tick enters `schedule` with `current` = E, and
`reap_sweep` — which runs at the top of every `schedule` and frees any zombie that
is not the running task — finds D and frees its address space. That is D's
`reap (sweeper):` line. The shell's `SYS_WAIT` runs only afterwards, finds D's
address space already gone, and quietly collects the tombstone alone, so **D
produces no `reap (wait):` line at all, even though it was waited on.**

That outcome hangs entirely on the task-table order and on how `find_next_ready`
walks it. Reorder the table, or change the scan so the shell is reached before E
when D exits, and D would be reaped by the shell's `SYS_WAIT` and its line would
read `reap (wait):` instead. Treat the labels here as a fact about today's
scheduler, not a guarantee: if the table or `find_next_ready` changes, work out
again which path frees D before trusting this output.

Two more things the run shows. The prompt returns while E is still printing,
because the shell waited for D and not for E and stays usable throughout. And D's
line reports fewer free frames than E's (30519 against 30591 here): when D is swept
E is still running and holding its own address space, and E's line is the count
coming back to the baseline once E is gone too.

## E.ELF — the orphan

Prints `E` fifteen times, then exits with status 7. **Nobody will ever read that
7.** By the time E exits, D is long gone, so E's tombstone has no reader and the
sweeper drops it rather than keeping a fact nobody can ask for. Seven is distinctive
purely so that it would be obvious, rather than plausible, if it ever did turn up.

Run on its own (`run e.elf`) it is an ordinary short program reaped by the shell's
wait, and status 7 is duly printed. It is only interesting as D's child.

## F.ELF — the multi-cluster write

Writes a 16KB file, `FTEST.TXT`, and checks it read back. The shell's own `write`
can only make a single-cluster file (a typed line is far shorter than a 512-byte
cluster), so the write path's real work — allocating and linking a 32-cluster chain
and reading it back in order — is only ever exercised here.

It is self-checking, so nobody eyeballs 16KB. It fills a buffer with numbered,
fixed-width lines (the HUGE.TXT idea), writes it, reads it back into a second
buffer, and compares byte for byte and on length. It exits **0** only on an exact
match, and with a distinct non-zero status otherwise, so `run: f.elf exited with
status N` names the failure: 1 write failed, 2 read failed, 3 length mismatch, 4
content mismatch.

    > run f.elf
    run: started f.elf
    F: FTEST.TXT 16384 bytes written and verified
    run: f.elf exited with status 0

16KB is deliberate: big enough to be a real 32-cluster chain, small enough to stay
under the shell's 32KB read buffer, so afterwards `read FTEST.TXT` still prints the
whole thing for a human spot check. On the host, `mtype -i disk.img ::/FTEST.TXT |
wc -c` reports 16384.

## Why every loop is bounded

There is no way to kill a task and there are no signals. A program that never exits
leaves its parent blocked in `SYS_WAIT` with no way back short of a reboot, so
`run <that program>` would make the shell permanently unusable. Every program here
therefore runs a fixed number of rounds and calls `sys_exit` at the bottom. There is
no crt0 either, so falling off the end of `_start` is undefined behaviour rather than
an implicit `exit(0)`: the call has to be written out.
