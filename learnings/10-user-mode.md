# Chapter 10: User Mode

> Read chapter 9 (the GDT and the TSS) first. That chapter built two pairs of
> descriptors, one at privilege 0 and one at privilege 3, and said the kernel and
> user boundary *is* those entries. Nothing has ever used the ring-3 pair. This
> chapter uses them.

## Where we are

Every line of code in the machine currently runs at ring 0. The kernel, the
drivers, the shell, all of it. Which means every line of code can do anything: read
any memory, write any memory, halt the processor, disable interrupts, reprogram the
page tables.

That is fine while you are the only person writing the code and there are eight
hundred lines of it. It stops being fine the moment you want to run a program you
did not write, or a program you wrote badly.

## Rule 1: the kernel gives up power deliberately

Here is the strange thing this chapter does, stated plainly so the strangeness does
not slip past.

The kernel is about to arrange for a program to run **less capably than the code
that started it**. It will hand control to that program having first made sure the
program cannot do most of what the kernel can do. On purpose. As the point of the
exercise.

Why would you build a machine that can do anything and then go to considerable
trouble to make most of the code on it unable to?

Because "can do anything" and "will do the right thing" are different properties,
and only the first one is under your control. A bug in code running at ring 0 is not
a bug in that program. It is a bug in the machine. A stray pointer does not crash
the program, it corrupts the kernel, and the machine dies later somewhere else with
no useful information about why.

Dropping to ring 3 converts a whole category of "the machine is now subtly wrong"
into "that program stopped."

## Rule 2: the hardware enforces it, not the kernel

This is the part worth being precise about, because the intuitive model is wrong.

The kernel does **not** watch what a program does. It cannot. Watching would mean
running alongside it, and there is one CPU: while the program runs, the kernel is
not running at all. There is nobody home to check anything.

So the kernel does something better. Before handing over control, it arranges the
machine so that **the CPU itself refuses.** The check is not a comparison in kernel
code that runs sometimes. It is in silicon, on every single memory access and every
single instruction, always, at no cost.

That is the trade at the heart of this chapter: you give up the ability to inspect
what a program is doing, and in exchange you get certainty about what it cannot do.

## Rule 3: what ring 3 loses, and what it keeps

Concretely, a ring-3 program **cannot**:

- Execute privileged instructions. `hlt`, `cli`, `lgdt`, `ltr`, writes to control
  registers, `in` and `out`. Attempting one is a general protection fault.
- Touch a page whose user bit is not set. Not read it, not write it, not execute it.
- Raise its own privilege. There is no instruction for it. None. This is not a
  matter of the kernel hiding something, the capability does not exist.

And it **keeps**:

- Arithmetic, control flow, its own registers.
- Its own code and its own stack.
- One door, which is chapter 11.

That is a program. Everything else has to be asked for.

## Rule 4: permissions AND down the walk

The mechanism for the second bullet is worth understanding because it is elegant
and slightly counter-intuitive.

Each entry in the page table tree carries a **user bit**. The CPU grants ring-3
access only if that bit is set at *every* level from the top of the tree to the
leaf. The bits are ANDed down the walk.

So permissions do not OR. A permissive parent can never grant what a restrictive
child denies. Which means it is perfectly safe to be generous high up in the tree,
and TownOS is:

```
PML4[0], PDPT[0]           user bit SET     permissive: a user walk may pass through
PD[0]   0x000000-0x1FFFFF  user bit CLEAR   kernel code, data, VGA, kernel stack
PD[1]   0x200000-0x3FFFFF  user bit CLEAR   kernel
PD[2]   0x400000-0x5FFFFF  user bit SET     ring-3 code
PD[3]   0x600000-0x7FFFFF  user bit SET     ring-3 stack
```

Setting the user bit on the top two levels looks alarming until you see the AND.
Those entries do not grant anything, they merely decline to block. The leaves are
where the decision is actually made, and PD[0] and PD[1] withhold it.

**The leaf is the real gate.** That is the sentence to keep.

## Rule 5: you cannot jump into ring 3

Now the mechanics, and they contain a genuine surprise.

There is no instruction that lowers privilege. You cannot `jmp` into ring 3, you
cannot `call` into it, there is no `enter_user_mode` opcode.

What x86 has is the *other* direction. Privilege goes **up** by interrupt or fault,
and comes back **down** by returning. Everything is built around that pair. So the
only way into ring 3 is to return into it.

Which is a problem the very first time, because you have not been there yet. There
is nothing to return from.

## Rule 6: forge the frame

So you fake it.

`iretq` is the instruction that returns from an interrupt. It pops five values off
the stack in a fixed order: RIP, CS, RFLAGS, RSP, SS. It does not know or care
whether a real interrupt put them there.

So `enter_user_mode` pushes those five values by hand, in reverse, and runs
`iretq`:

```
push  SS      = 0x23        ring-3 data selector
push  RSP     = 0x800000    top of the ring-3 stack
push  RFLAGS  = 0x202       reserved bit 1, plus the interrupt flag
push  CS      = 0x1B        ring-3 code selector, requested privilege 3
push  RIP     = the program's entry point
iretq
```

The CPU pops that frame, sees a code selector requesting ring 3, performs a
privilege change, loads the new stack, and begins executing the program.

Note what this is and is not. It is not a hack around the hardware. It is the
hardware's own intended mechanism, invoked from a standing start. The very first
drop into ring 3 is the only one with no matching interrupt behind it. Every
subsequent entry into a task is a real return, performed by the scheduler restoring
a frame that a real interrupt genuinely pushed.

This is the second time this trick has appeared. Chapter 9 reloaded CS with a far
return because the far jump encoding does not exist in 64-bit mode. Same idea:
**when the architecture only gives you a way back, arrange to be coming back.**

## Rule 7: the one bit that keeps you in charge

`RFLAGS = 0x202` sets the interrupt flag, and that is not tidiness.

If a program ran with interrupts masked, the timer would not tick and the keyboard
would not respond. Worse, once a scheduler exists, the running task would never be
preempted, because preemption *is* the timer interrupt. The program would own the
machine until it chose to give it back, which is exactly the thing you built ring 3
to stop depending on.

Now notice the loop that closes here. Preemption depends on an interrupt arriving.
A program that could mask interrupts could refuse to be preempted. Which is
precisely why `cli` is a privileged instruction.

**The one bit that makes preemption possible is a bit ring 3 cannot clear.** The
architecture thought about this.

## Rule 8: how you know the drop is real

Worth stating, because the obvious test proves nothing.

A ring-3 program that prints "hello" and exits is indistinguishable from a ring-0
program that prints "hello" and exits. Success proves nothing about privilege,
because everything it did was allowed either way.

The proof is a program doing something **forbidden** and being stopped. Reach for a
kernel address and take a page fault. Execute `hlt` and take a general protection
fault. The evidence that the boundary exists is the fault, not the output.

That is a general principle for this kind of work: a protection mechanism is only
demonstrated by the thing it refuses.

## Rule 9: the door

Once a program is down there, it can compute and it can touch its own memory. It
cannot print, cannot read a file, cannot start another program, cannot exit
cleanly. All of that is privileged.

So the only remaining question is how it asks. That is chapter 11, and it is one
instruction wide.

## What this still is not

- **No isolation between programs.** There is one page table tree, so every ring-3
  program shares the same user region. They cannot reach the kernel, but they can
  reach each other. That is chapter 14, and it is the difference between "programs
  cannot break the kernel" and "programs cannot break each other."
- **No way to survive a misbehaving program.** A ring-3 fault is caught, but the
  handling is crude. A real kernel kills the offending task and carries on.
- **No resource limits.** A ring-3 program cannot corrupt the kernel, but it can
  still loop forever, allocate until nothing is left, or spawn endlessly. Privilege
  separation is about capability, not about fairness.

## Exercises

1. A ring-3 program executes `cli`. Describe what happens, and then describe what
   would happen to the whole machine if `cli` were not privileged.
2. The user bit is set on PML4[0] and PDPT[0]. Explain why that does not expose the
   kernel, in terms of how the walk combines the bits.
3. There is no instruction that lowers privilege, only returning. Why do you think
   the architecture was built that way? Argue it from what interrupts have to do.
4. `enter_user_mode` pushes five values and runs `iretq`. Which of the five would
   you most expect to get wrong, and what would the symptom be?
5. A program prints "hello" and exits normally. Explain why this is not evidence
   that the drop to ring 3 worked, and design a test that is.
6. Suppose `RFLAGS` were forged as `0x002` instead of `0x202`. Describe the machine
   thirty seconds later.
7. Ring 3 cannot raise its own privilege, and the kernel does not check what ring 3
   does. Given both, explain how the kernel ever regains control of the CPU.
