# Chapter 19: Blocking and Sleep

> Read chapter 12 (the scheduler) first. This chapter adds one value to an enum in
> that chapter's code and then spends itself on the consequences.

## Where we are

Here is how the shell reads a key. It asks the kernel for one. The kernel says
there isn't one. It asks again.

Over six seconds of you not touching the keyboard, that loop issued **362,648
system calls**. Six seconds of a machine running flat out, at full power, with the
fan going, to establish that nothing happened.

The task was not stuck. It was not broken. It was doing exactly what it was written
to do. That is the uncomfortable part.

## Rule 1: waiting is not an activity

A busy-wait is a program *doing* something in order to *not* do something. And from
where the scheduler stands, there is no difference at all between a task computing
prime numbers and a task asking "any keys yet?" four hundred thousand times. Both
are `TASK_READY`. Both get a timeslice. Both look busy, because both *are* busy.

The kernel has no vocabulary for "I have nothing to do." That is the actual missing
feature. Everything else in this chapter follows from adding one.

## Rule 2: waiting is a state, not a loop

Add one value to the task state enum:

```
TASK_BLOCKED
```

A blocked task is skipped by the scheduler entirely. Not given a short timeslice.
Not polled. Skipped, as though it were not there, until somebody says otherwise. It
costs exactly nothing.

That is the feature. One enum value. Everything for the rest of this chapter is
bookkeeping around it, and every piece of that bookkeeping exists because of a
question the enum value raises and does not answer.

## Rule 3: you have to say what you are waiting for

First question: when something happens, who wakes up?

If "blocked" is one undifferentiated state, the only possible answer is "everyone",
and then each woken task checks whether the thing it wanted actually happened and
most of them go straight back to sleep. That is the busy-wait again, with more
steps and worse timing.

So a blocked task records **why**:

```
WAIT_NONE
WAIT_KEY
```

`WAIT_KEY` means "I am waiting for a keystroke." Later, in chapter 20, `WAIT_CHILD`
means "I am waiting for a child to finish." Every future kind of waiting adds one
more value here and nothing else.

The reason is what makes waking *selective*. Without it there is no way to wake the
right task, and waking the wrong one is indistinguishable from not sleeping at all.

## Rule 4: whoever causes the event wakes the waiters

Second question, and this is the one that decides the architecture: whose job is it
to notice that a key arrived?

Not the sleeping task's. **A sleeping task cannot notice anything**, because
noticing requires running, and running is exactly what it has stopped doing. That
is not a limitation to work around, it is the whole point.

Not the scheduler's either, and this is the tempting wrong answer. You could have
`schedule()` check "has a key arrived?" on every tick and wake the waiters if so.
That works. It is also the busy-wait moved into the kernel, where it now runs
whether or not anybody cares about keys.

So: **the thing that causes an event is the thing that wakes the waiters.**

A key arriving is an interrupt. The keyboard IRQ is the only code in the machine
that knows, at the moment it becomes true, that a key has arrived. So the keyboard
IRQ pushes the character into the buffer and then calls `scheduler_wake(WAIT_KEY)`.
Nobody polls anything. Nobody checks. The fact is delivered by the same event that
created it.

That pairing rule holds for everything that comes after. `exit` wakes `WAIT_CHILD`.
A disk completion interrupt would wake `WAIT_DISK`. A pipe write wakes
`WAIT_PIPE`. Once you see it, most of a kernel is this one shape wearing different
clothes.

Note what waking does and does not do. It sets the task back to `TASK_READY` and
returns. It does not switch to it. The choice of what runs next stays where it
belongs, in the scheduler on the next tick, and the interrupt handler stays short.

## Rule 5: make it true before you announce it

Ordering, and it is one line of code with a real bug behind it.

The keyboard handler pushes the character into the buffer *first*, and calls
`scheduler_wake` *second*.

Do it the other way and the woken task can be scheduled, re-issue its read, find an
empty buffer, and block again. One keypress turns into a wasted round trip and the
character arrives on the next one.

The general form: **do not announce an event until the thing that makes it true has
happened.** Chapter 21 hits the same rule from the other end, where publishing a
directory entry before the data exists produces a file that claims things that are
not so. Same idea. Announce last.

## Rule 6: when nobody is runnable at all

Third question, and it did not exist before this chapter: what does the scheduler do
when every task is blocked?

Before blocking, this was impossible. Somebody was always ready, even if all they
were doing was asking about keys. Now the shell can be asleep and there may be
nothing else on the machine.

The tempting answer is to loop, checking for someone to run. That is the busy-wait
for the third time, now in the deepest and most expensive place available.

The right answer is a CPU instruction. `hlt` stops the processor until an interrupt
arrives. Not a loop that goes around very fast. An actual stop. Power draw drops,
the fan slows, and the machine sits there costing nothing until a key is pressed or
the timer ticks.

### The trap: `hlt` with interrupts off is forever

`hlt` waits for an interrupt. If interrupts are disabled, no interrupt can arrive,
and the machine is stopped permanently with no message and no way back. It looks
exactly like a crash and it is much harder to diagnose than one.

You enter `schedule()` from an interrupt gate, which means the CPU has already
cleared the interrupt flag. So the idle loop must enable interrupts before halting:

```c
while (!any_task_ready()) {
    __asm__ __volatile__("sti; hlt; cli");
}
```

Enable, halt, and on waking disable again before looking at the task table.

### The subtler trap: the lost wakeup

Look at why `sti` and `hlt` have to be *adjacent*.

Suppose they were not, and an interrupt landed in the gap between them. The
interrupt runs, wakes a task, and returns. Then `hlt` executes and the machine
stops, waiting for a wakeup that has already been spent. Everything is now ready to
run and nothing ever runs again.

x86 handles this deliberately: **`sti` does not take effect until after the
following instruction.** The pair is atomic by design of the instruction set,
specifically so that this exact loop can be written safely. That is not an accident,
it is a feature put there for people writing this loop.

Which is also why the two instructions live in one `asm` statement rather than two.
Separate them and a compiler is free to put something between them.

### The trap after that: not nesting

The idle loop runs with interrupts *enabled*. That is the entire point of it. But it
means timer ticks keep arriving while you sit there, and every timer tick calls
`schedule()`.

So `schedule()` gets called again while an earlier `schedule()` is parked inside the
idle loop. If that nested call also parked in the idle loop, every tick would nest
one level deeper on the single shared kernel stack, and a long idle would quietly
overflow it. Leave the machine alone for a minute and it dies.

The fix is a flag, `scheduler_idling`. While it is set, a nested `schedule()` does
nothing and returns. The nesting depth stays at one, the nested tick unwinds back
into the `hlt` loop, and the *outer* call, which owns the register pile that is
going to be overwritten, is the one that performs the eventual switch.

Three traps, all in about eight lines. This is the densest code in the kernel per
line, and none of it is complicated. It is just unforgiving.

## Rule 7: where does a woken task resume?

The last question, and the one that forced the design.

The obvious answer is: right where it stopped. It was in the middle of the
`readkey` syscall, so thaw it there and let it carry on.

You cannot. Not "it is hard", not "it is slow". It is **not representable**, for two
independent reasons, either of which alone would be enough.

**The saved rip is always a ring-3 address.** A task's register pile holds what the
CPU pushed on the ring-3 to ring-0 transition. So restoring a pile can only ever
resume user code. There is no way to write down "was halfway through a C function
in the kernel."

**There is one kernel stack, shared by every task.** The C frames you are standing
in right now are abandoned the instant you switch away, and the next task to enter
the kernel writes straight over them. There is nothing to come back to.

### So do not resume. Re-issue.

The CPU pushed the address of the instruction *after* `int 0x50`. Wind it back by
the length of that instruction, and it now points at the `int` itself.

```c
#define INT_INSTR_LEN 2      // the opcode is CD ib: one byte opcode, one byte vector
r->rip -= INT_INSTR_LEN;
```

When the task is eventually woken and scheduled, its `iretq` lands *on* the `int`,
the syscall runs again from the very top, and this time the buffer has something in
it.

Instead of resuming in the middle, you resume at the beginning.

### What that demands in exchange

**A syscall that can block must be safe to run twice.** Running `readkey` and
finding nothing must leave the world exactly as it found it, because it is about to
run again. It does, since finding an empty buffer changes nothing. Every future
blocking syscall inherits this requirement.

**And you must not write a return value on the blocking path.**

This one is nasty. The syscall number arrives in `RAX`. When the rewound `int 0x50`
executes again, the CPU reads the number out of `RAX` again. So if you write a
return value into `RAX` before blocking, the woken task issues a *different
syscall*. Write 0 there and it issues `SYS_EXIT` and kills itself.

Sit with the shape of that bug for a second. A return value that corrupts the
*next* call rather than the current one, on a code path that only executes when the
buffer happens to be empty. It is a good argument for the comment that sits above
that line in `syscall.c`.

## What it bought

362,648 syscalls over six idle seconds became **3**.

Not three hundred. Three: the banner, the prompt, and the `readkey` that blocks. The
machine then sits in `hlt` doing nothing at all until you touch a key.

## The payoff you have not seen yet

Everything in rule 7 was forced on you by a constraint. One kernel stack, ring-3
rips only. It looks like a workaround.

It is the best thing in the chapter, and chapter 20 is where you find out why.

When a parent waits for a child, it blocks on `WAIT_CHILD`. The child exits, wakes
the parent, and the parent's re-issued `wait` runs from the top, finds the child
already dead, reads its exit status, and returns. No resume path. No state saved
inside the syscall. No "where was I."

Every future kind of waiting gets that for free. Waiting on a pipe, on a disk, on a
lock. The constraint turned out to be the design.

## What this still is not

- **No timeouts.** There is no way to say "wait for a key, but give up after 100
  milliseconds." That needs the timer to be a waker too, and a per-task deadline.
- **No waiting on several things at once.** A task waits for exactly one reason. The
  Unix answer to this is `select` and `poll`, and it is a genuinely different shape.
- **The wake is a broadcast.** `scheduler_wake(WAIT_KEY)` wakes every task waiting on
  a key, and only one of them can have the character. The rest wake, find nothing,
  and block again. That is a thundering herd, and it does not matter at all with one
  or two tasks. It matters enormously with a thousand.
- **No priority.** A woken task goes to the back of the round-robin like everybody
  else. A task that has been asleep for ten seconds waiting for you to press a key
  gets no more urgency than one that has been computing all along.

## Exercises

1. A task blocks on `WAIT_KEY`. Before it is scheduled again, three keys are
   pressed. Trace what happens to all three characters and what the task sees when
   it finally runs.
2. Why can the scheduler not simply check the keyboard buffer on every tick and wake
   `WAIT_KEY` tasks itself? It would work. Give the argument against it in terms of
   what runs when nobody cares about keys.
3. Rewrite the idle loop as `sti;` then `hlt;` in two separate `asm` statements.
   Describe the exact interleaving that hangs the machine, and say how often you
   would expect to hit it.
4. `scheduler_idling` makes a nested `schedule()` return immediately. What goes wrong
   without it, how long does the machine survive, and why is the failure so hard to
   attribute?
5. Suppose the saved `rip` were rewound by 1 byte instead of 2. What executes when
   the task is woken?
6. A new blocking syscall is added that reads a byte and also advances a counter
   before checking whether data is available. Explain why the re-issue design breaks
   it, and what the fix is.
7. `SYS_READKEY` must not write to `RAX` on the blocking path. Name every other
   register that would be equally dangerous to write there, and say how you decided.
8. A task blocks on `WAIT_KEY` and no key is ever pressed. Describe the machine's
   power consumption, and then describe the same scenario in the version of this
   kernel that existed before this chapter.
