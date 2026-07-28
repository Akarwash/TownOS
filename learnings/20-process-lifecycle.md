# Chapter 20: Process Lifecycle

> Read chapter 19 (blocking and sleep) first. Everything in this chapter is built
> on the parking primitive from that one. If "a task can be taken out of the
> rotation and put back later" is not solid yet, go back.

## Where we are

The town has residents. It can create them, give each one a street of their own,
switch between them, and park one that has nothing to do.

It cannot kill anyone.

Look at what your own scheduler says about itself. `num_tasks` only ever goes up.
Ids are handed out `0, 1, 2, ...` and never reused. The comment in
`paging_create_address_space` shrugs off a leak on a failed create with "tasks are
never destroyed once built", and it is right, they aren't. Every program you have
ever run since boot is still standing in the rotation, getting tapped on the
shoulder by the timer forty times a second, doing nothing, holding its street and
all its boxes.

Run twenty programs and you are out of memory. Not because twenty programs is a
lot, but because none of the nineteen that finished ever gave anything back.

This chapter is the town learning to take things back.

## What a resident owns

New word, and it is worth being precise about it. A **resident** is a task. Not a
building, a person. A resident owns exactly three things:

1. **A street.** Their address space. Concretely: a PML4 frame, a PDPT frame, a
   PD frame, and the page tables that got created under PD[2] and PD[3] the first
   time they touched their code and their stack.
2. **Boxes on that street.** Their frames. Code, data, stack.
3. **A line on the clipboard.** Their `task_t`, sitting on the kernel heap, with a
   pointer to it in `tasks[]`.

Death means giving back all three. Hold that list in your head, because the rest of
the chapter is really just an argument about the order you can give them back in.

## Rule 1: a resident has to announce their own death

The obvious question first. Why can't the town just *notice* that a program is
done?

Because there is nothing to notice. A program is a pile of instructions and the
finger walks through them. When it reaches the last one it does not stop, it walks
into whatever bytes happen to be sitting there next and executes those. There is no
end-of-program marker. There is no "done" the hardware understands. Your `A.c` ends
in `for (;;)` and that is not laziness, it is the only honest thing a program could
do in a town with no way to die.

So finishing has to be a deliberate act. The resident walks up to the town hall
(`int 0x50`) and says "I am done."

That announcement is a syscall, and you already have the number for it. `SYS_EXIT`
is 0 in your `syscalls.h`, and today it means something completely different: it
halts the whole machine. That made sense back when there was one program and no
scheduler to return to. It has been wrong for about eight rungs. We are not adding
a syscall here so much as finally giving an old one the meaning its name always
claimed.

## Rule 2: they leave a number behind

`exit` carries one value with it. The **exit status**. Zero means it went fine,
anything else means it did not, and by convention the "anything else" is a small
number the program picks to say *how* it went wrong.

That number is small, but it is the reason death cannot be one clean sweep.

Think about what you want `run A.ELF` to do. You want the shell to launch A, wait,
and then tell you how it went. If the town erased A completely at the instant A
said "I am done", there would be nothing left to ask. The answer would have been
destroyed by the same act that produced it.

So the number has to outlive the resident. Which means:

**Death is two events, not one.**

- **Dying.** The resident says "done". The town writes their number on a
  tombstone and takes them off the rotation. They never run again.
- **Burial.** Somebody else reads the tombstone, and only then clears the plot.

Everything hard about this rung lives in the gap between those two events.

## Zombies

A resident in that gap has a name, and it is a good one. They are a **zombie**.

A zombie is dead. It is not in the rotation, the scheduler steps straight over it,
it will never execute another instruction. But it still holds a line on the
clipboard, with a number written on it.

The instinct is to call that a leak. It is not. It is the tombstone. It is the
whole point. A `task_t` is a few hundred bytes of kernel heap, and a zombie can sit
there for as long as it takes for someone to come and read it.

Compare that to what a zombie *should not* hold: a street and a pile of boxes.
Those are megabytes. Those we want back immediately, and this is the first hint of
the shape of the answer. The two events do not have to give back the same things.

## Rule 3: `wait` is the parent reading the tombstone

Who does the reading? Whoever started them.

Your shell runs a program. So the shell is the **parent** and the program is the
**child**. That relationship is real and it has to be written down at birth, which
means a new field on `task_t`: who my parent is. Nothing in the town remembers who
launched what today, because nothing has ever needed to.

Task 0, the shell itself, was started by `kernel_main` before any resident existed.
It has no parent. That is a genuine case, not an edge case, and it gets a sentinel
value.

`wait` is a resident saying "I am doing nothing else until my child is done". Two
cases, and they are worth keeping separate in your head:

**The child is already dead.** The tombstone is sitting right there. Read the
number, clear the plot, carry on. No waiting happens at all. `wait` is not a
blocking call in this case, it is a lookup.

**The child is still alive.** Now the parent parks. This is chapter 19's machinery,
unchanged: state goes to `TASK_BLOCKED`, the reason is `WAIT_CHILD`, the scheduler
steps over them. The waker this time is not the keyboard interrupt. It is `exit`
itself. The event that causes the wake is a child dying, so the code that handles a
child dying is the code that does the waking. That is the same pairing rule you
already wrote down in `scheduler_wake`: whoever causes an event wakes the waiters.

### The payoff from how you built blocking

This bit is worth stopping on, because it is a piece of design luck that came from
a constraint.

Back in chapter 19 you could not freeze a task mid-syscall. Two reasons: the saved
rip is always a ring-3 address, and there is one shared kernel stack, so the C
frames you are standing on get overwritten the moment you switch away. So instead
of resuming in the middle, you rewound the finger by two bytes back onto the
`int 0x50` and let the woken task *re-issue the whole syscall from scratch*.

Now look at what that does to `wait`.

The parent calls `wait`. Child is alive, so the parent parks with the finger
pointing at the `int`. Later the child exits and wakes the parent. The parent is
rescheduled, the iretq lands on the `int 0x50`, and `wait` runs again from the top.
This time it takes the first branch: the child is a zombie, the tombstone is right
there, read it and return.

No resume path. No saved state inside the syscall. No "where was I". The re-arm
trick, which you built for the keyboard, handles the parent-child case for free,
and it will handle every future `wait` reason for free too. That is what it looks
like when a constraint turns out to have been a good idea.

## Rule 4: nobody buries themselves

Here is the hard part.

When a resident calls `exit`, we are in the kernel. But CR3 has not changed. It
still points at the dying resident's street.

Kernel code keeps running fine, and it is worth being clear about why. You clone
the kernel half into every tree **by value**: every task's PD holds copies of the
same huge-page entries pointing at the same physical kernel memory. So kernel
`.text`, the kernel stack, the `tasks[]` array, all of it is reachable through
whatever tree happens to be loaded. That is the invariant that makes task switching
possible at all, and it is doing its job right now.

But the thing we want to burn is the map the CPU is using to find its way around.

Hand the PML4 frame back to the pool and it goes on the free list. The next
`task_create_from_file` calls `alloc_table`, gets that exact frame, `memset`s it to
zero, and now CR3 points at 512 zeroes. The next TLB miss walks that and finds
nothing present. Triple fault. And the machine will have been running fine for
several seconds by then, so the crash will look like it came from somewhere else
entirely.

Same problem one level up. The `task_t` we would be freeing is the one whose `exit`
we are currently servicing.

You cannot saw off the branch you are sitting on. So:

**The dying resident does the paperwork, never the shovel.**

Set the state. Write the number. Wake the parent if it is waiting. Then hand the
town over to the scheduler and never come back. `schedule()` picks somebody else
and loads *their* CR3. Now the dead street is not underfoot, it is just a pile of
frames nobody is standing on, and burial is safe.

## Who holds the shovel

Three candidates, and only three. The dying resident, the parent, or the town.

**The dying resident** is out. That is the rule we just wrote.

**The parent, at `wait` time.** Tempting, and it is the classic Unix answer. The
parent is running on its own street, so the dead street is not underfoot. One
place, one moment, easy to reason about: read the number, then free everything.

The flaw is that it makes a whole street hostage to somebody else's manners. If the
parent never calls `wait`, the child's code frames, stack frames and page tables sit
allocated forever. That is not much of an upgrade on "tasks are never destroyed".
It just changes the reason.

**The town, one tick later.** The dying resident sets their state and yields.
`schedule()` runs, switches CR3 to somebody else, and the dead street becomes
ordinary unused frames. So the *next* time `schedule()` runs, it can sweep: walk
the clipboard, find anyone marked dead, and if they are not the resident currently
on the CPU, bury the heavy stuff.

That "if they are not the current resident" test is the entire safety argument, and
it is one comparison. The cost is one timer tick of latency, which is 25
milliseconds of a few frames staying allocated. Nothing.

## Where that lands

The two jobs split, and this split is what real kernels do:

**The sweeper takes the heavy stuff.** The street and the boxes. It runs inside
`schedule()`, skips whoever is currently on the CPU, and frees the user frames,
then the page tables holding them, then the three tables above, then the
`address_space_t` handle. Memory comes back promptly no matter what anyone else
does.

**The parent takes the light stuff.** The `task_t` and the clipboard slot, at
`wait` time, after reading the number. That is burying the tombstone.

So a zombie in your town, one tick after it died, is a resident with no street, no
boxes, and one line on the clipboard with a number on it. It costs a few hundred
bytes and it can sit there all day.

### What the sweeper must not touch

Your PD holds two kinds of entry side by side. Two user slots (PD[2] for code,
PD[3] for the stack) pointing at page tables this resident owns, and hundreds of
kernel slots that are **copies of the boot table's huge-page entries**, pointing at
physical memory every single resident shares.

A sweeper that loops over all 512 PD entries freeing whatever it finds will free
the kernel out from under the entire town. The first task to run after that is
dead, and so is the second, and there is no message.

So the sweeper frees exactly the two user branches, by index, and nothing else. Not
"everything that is present". Not "everything except huge pages", though checking
the huge bit as well is a cheap second lock on the door. Two known indices. This is
the one place in this rung where being clever is actively dangerous.

## Orphans

The obvious objection: what if the parent dies first?

Then nobody will ever read the tombstone. The clipboard fills up with lines that
will sit there until reboot. Not catastrophic (the streets and boxes were already
swept, so it is only a few hundred bytes each) but it is an unbounded leak and
unbounded leaks are how kernels die.

Two fixes.

**Reparent.** When a resident dies, hand their children over to task 0. Task 0
becomes the town's undertaker of last resort, and eventually gets a loop that reaps
whatever lands on it. This is what Unix does with init, and it is why every orphaned
process on a Linux box ends up parented to PID 1.

**Skip the tombstone.** At sweep time, check whether the dying resident still has a
living parent who could possibly come and read the number. If not, there is no
point leaving a tombstone for a reader who does not exist. Free the `task_t` too and
null the slot.

TownOS does the second one, for now. It is less code and it introduces no new
concept, and the case it handles (the shell outliving everything, which is the only
shape the town currently has) is handled exactly right. Reparenting is a clean
bolt-on for the day there is a real init, and that day is not this rung.

## Holes in the clipboard

One consequence worth naming because it will bite you.

Freeing a slot means `tasks[]` now has NULL holes in it. Every loop in
`scheduler.c` that walks `0..num_tasks-1` and dereferences `tasks[i]` without
checking has just become a page fault waiting for the right timing:
`any_task_ready`, `find_next_ready`, `scheduler_wake`. They were all written under
an invariant ("slots 0 to num_tasks-1 are exactly the live tasks") that this rung
deletes.

And we do **not** reuse ids. A freed slot stays NULL forever and `num_tasks` becomes
a high water mark rather than a count. Reusing ids invites the classic bug where
you wait on id 3, id 3 gets freed and handed to a completely different program, and
you are now waiting on a stranger. Ids are cheap. Confusion is not.

## What this buys you

`run A.ELF` finally behaves like a command instead of a background launch. The
shell starts A, parks (costing zero CPU, thanks to chapter 19), A runs to
completion, A exits with a status, the shell wakes, prints the status, prompts.

That is the first time TownOS does the thing every operating system does a thousand
times an hour. And you can run it in a loop forever without the machine slowly
filling up, which is the actual test.

## Exercises

1. A program falls off the end of `_start` without calling `sys_exit`. Describe
   exactly what the finger does next, and why the kernel cannot detect it.
2. Why can the sweeper not simply run at the top of `task_exit`, before yielding?
   Name the specific frame that makes it unsafe and the specific later moment the
   damage would show up.
3. The sweeper skips the resident currently on the CPU. Trace what happens to a
   zombie that is skipped, tick by tick, until it is finally swept. How many ticks?
4. Why does `wait` need to free the child's address space itself, given that the
   sweeper already frees address spaces? Under what timing would the sweeper not
   have got there first?
5. A parent has three children. Two are still running, one is a zombie. Walk
   through what a single `wait` call does, and then what a second one does.
6. Explain why `task_exit` must wake the parent *before* it calls `schedule()`, and
   what would happen on a town with only those two residents if it did it after.
7. The exit status is masked to 0..255 before being stored. `wait` returns -1 when
   the caller has no children. What ambiguity does the mask remove, and what would
   break without it?
8. Suppose the sweeper looped over all 512 PD entries instead of the two user
   indices. Describe the first symptom you would see in QEMU and explain why it
   would not point you anywhere near the sweeper.
