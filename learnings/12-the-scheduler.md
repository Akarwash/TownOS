# Chapter 12: The Scheduler

> Read chapter 3 (interrupts) and chapter 10 (user mode) first. This chapter turns
> out to be mostly a reuse of chapter 3, in the same way chapter 11 was.

## Where we are

There is one CPU. It executes one instruction stream. That is not a limitation of
your kernel, it is what the hardware is.

And you want the machine to run several programs.

## Rule 1: it is an illusion, and the illusion is the product

Nothing is going to run at the same time as anything else. The CPU is going to run
one program for a few milliseconds, then another, then another, fast enough that a
person cannot tell the difference.

Say that plainly rather than treating it as an embarrassing detail, because it is
the actual definition. **Multitasking on one core is time-sharing.** It is not a
simulation of something better. It is the thing itself.

And it does not stop being the thing on better hardware. A real machine with eight
cores running two hundred processes is doing exactly this, eight at a time. The
trick does not go away, it just gets wider.

At 100 switches a second, a program that gets every third slot still feels
continuous to a human, because a human cannot perceive a 20 millisecond gap in
anything.

## Rule 2: a task is a pile of numbers

Here is the conceptual move the whole chapter rests on, and it is genuinely
surprising the first time.

A program that is not currently running still exists. Where?

Not on the CPU. The CPU is running something else, and its registers hold that
other program's values. So everything that made your program *be* the running
program has to have been written down somewhere.

What is on that list?

- The fifteen general-purpose registers.
- The instruction pointer, which is where it was up to.
- The stack pointer, which is where its stack is.
- The flags.
- A pointer to its map, which is CR3.

That is the complete list. There is nothing else.

So: **a task is a pile of saved numbers plus a map.** It is not a thing that lives
somewhere and is temporarily paused. It is data sitting in an array. Switching tasks
means choosing a different pile.

Sit with that for a second. The running program is not special. It is just the pile
that currently happens to be loaded into the registers.

## Rule 3: the pile already exists

Now the payoff, and it is why this chapter is short.

Chapter 3 built interrupt stubs. Every one of them saves all fifteen general-purpose
registers on entry and restores them on exit, and the CPU itself pushes the
instruction pointer, the flags, and the stack pointer. That machinery exists so that
an interrupted program resumes as though nothing happened.

Look at that list again. It is the same list.

**The interrupt frame is the context.** The save-and-restore mechanism a scheduler
needs was already built, for an unrelated reason, three chapters ago. You do not
have to write it.

## Rule 4: switching is editing, not moving

The interrupt handler receives a pointer to that saved frame. So to switch tasks:

1. Copy the frame into the current task's slot. That is "saving."
2. Copy the next task's slot over the frame. That is "restoring."
3. Load the next task's map into CR3.

Then return normally. The stub pops the registers and runs `iretq`, exactly as it
always does, and lands in a completely different program. It has no idea. It
restored what it was told to restore.

**You do not switch tasks. You lie to the return path about who it was serving.**

That is the trick, and it is worth admiring for a moment. There is no context-switch
instruction, no special mechanism, no assembly beyond what already existed. There is
a `memcpy` and a CR3 write.

## Rule 5: preemption is taking, not receiving

There are two ways a scheduler can get the CPU back.

**Cooperative:** wait for the program to call `yield`. This is simple and it is a
hope rather than a mechanism. A program with an infinite loop never yields. A
program with a bug never yields. A hostile program certainly never yields. One
program that does not cooperate takes the whole machine, permanently, and there is
nothing anyone can do about it because nothing else is running.

**Preemptive:** the decision is made by something the program does not control. The
timer interrupt fires, the handler calls the scheduler, and the scheduler switches.
The program did not agree. It does not know it happened. It cannot refuse.

Every tick is a preemption point here, so the quantum is one tick, 10 milliseconds.

And now chapter 10's loop closes one more time. The program cannot refuse to be
preempted because refusing would mean masking interrupts, and masking interrupts
means `cli`, and `cli` is privileged. **Preemption works because ring 3 exists.**
The two features are not independent, one is what makes the other enforceable.

## Rule 6: what preemption costs

Honest accounting, because it is not free.

**Switching costs time.** Copying two register piles and reloading CR3, which also
throws away the CPU's cached address translations. Switch too often and you spend
the machine on switching rather than on work. That is why the quantum is 10
milliseconds and not 10 microseconds.

**Everything shared becomes fragile.** Before preemption, kernel code ran to
completion. Now any data structure the kernel touches can be interrupted between any
two instructions, by anything. Every shared thing has to survive being cut in half
at an arbitrary point. This is the moment concurrency bugs become possible in your
kernel, and they never become impossible again.

**Timing becomes unpredictable.** A loop that took 10 milliseconds yesterday takes
40 today because two more programs exist. Nothing is wrong. That is just what
sharing means, and it is why real-time systems either avoid preemption or make
enormous promises about it.

## Rule 7: round robin, and why it is the right first answer

The policy here is the simplest one that works: pick the next ready task after the
current one, wrapping around at the end. Everybody gets an equal share, nobody
starves, and it is about ten lines.

Real schedulers are thousands of lines, and they exist because equal shares is often
the wrong answer.

Consider a text editor that wakes up when you press a key, does 200 microseconds of
work, and goes back to sleep, running alongside a compiler that has been flat out
for a minute. Equal shares treats them identically. What you actually want is for
the editor to run *immediately* every time it wakes, because it will not be greedy
and because you will notice the delay. The compiler will not notice anything.

Round robin cannot express that. It has no way to say "this one is more urgent" or
"this one has already had plenty." Everything after round robin is the story of
learning to say those things, and it is why Linux has been through several
schedulers and will be through more.

Round robin is the right first answer anyway, because it is obviously correct, and
you can only tell what a scheduler ought to prefer once you have programs with
different shapes to prefer between.

## Rule 8: two guards for untidy reality

Two details that are not conceptually deep but are the kind of thing that eats an
afternoon.

**The timer starts before the scheduler does.** The instant interrupts are enabled,
the timer begins ticking, and every tick calls the scheduler. That happens long
before any task exists. Those early ticks fire in kernel context, and there is
nothing to switch to and nothing to save: the "current task" is a forged entry
nobody has ever entered. Without a guard, the scheduler saves a kernel register
frame on top of a task that has not run yet, and the machine dies the first time it
tries to enter that task.

So there is a flag that stays clear until the scheduler is genuinely started, and
until then `schedule()` does nothing.

The general shape recurs constantly: **a subsystem that gets armed before it is
ready needs a flag saying so.**

**The first frame has to be forged.** A task that has never run has no saved pile,
because a pile is what an interrupt leaves behind and it has never been interrupted.
So when a task is created, the kernel manufactures the pile it *would* have had:
entry point in the instruction pointer, stack top in the stack pointer, ring-3
selectors, flags with the interrupt bit set.

After that, the first entry into a task is indistinguishable from a resume, and
there is no special case anywhere in the switching code.

This is the third time this trick has appeared. Chapter 9 reloaded CS with a forged
far return. Chapter 10 entered ring 3 with a forged interrupt return. Now a task
starts with a forged interrupt frame. **When the architecture only gives you a way
back, arrange to be coming back.**

## What comes later

Two things are missing from this chapter that the code has now, and both get their
own chapters:

- **A task that is waiting.** Right now every task is either running or ready to
  run, so a task with nothing to do burns its full share proving it. Chapter 19 adds
  a blocked state, and the scheduler learns to skip it.
- **A task that has finished.** Right now tasks are created and never destroyed.
  Chapter 20 adds exiting, and with it the question of who cleans up after a task
  that cannot clean up after itself.

Both are additions to the state enum in this chapter's code. The switching mechanism
does not change at all.

## What this still is not

- **No priorities.** Every task is equally important. There is no way to say
  otherwise.
- **No accounting.** Nothing tracks how much CPU anybody has used, so nothing could
  be fair even if it wanted to be.
- **No per-task quantum.** Everybody gets one tick.
- **One core.** Everything here assumes a single CPU, and a great deal of it stops
  being true on more.

## Exercises

1. List everything that has to be saved to make a task resumable, and then explain
   why the kernel does not need to save the task's memory contents.
2. The scheduler switches tasks by overwriting the saved interrupt frame in place.
   Explain what the interrupt stub thinks is happening.
3. A cooperative scheduler is simpler and faster. Give the strongest argument for it,
   and then say precisely which program breaks it.
4. `cli` is privileged, and preemption depends on the timer interrupt. Explain how
   those two facts are the same fact.
5. The quantum is 10 milliseconds. Describe the machine at a 10 microsecond quantum,
   and at a 10 second one.
6. Why must the scheduler ignore timer ticks that arrive before the first task has
   been entered? Describe the corruption if it did not.
7. A task's first frame is forged so that its first entry looks like a resume. What
   would the switching code need if it were not forged?
8. Round robin cannot prefer an interactive program over a batch one. Design the
   smallest change to this scheduler that could, and then say what your change
   breaks.
