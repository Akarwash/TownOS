# Chapter 11: System Calls

> Read chapter 10 (user mode) first, and chapter 3 (interrupts), because this
> chapter turns out to be almost entirely a reuse of chapter 3's machinery.

## Where we are

There is a program at ring 3. It can add numbers, follow branches, and touch its
own stack.

It cannot print. It cannot read a file. It cannot start another program. It cannot
even end itself cleanly. Every one of those requires touching hardware or kernel
data, and both are exactly what chapter 10 took away.

So a ring-3 program that cannot ask for anything is a program that can compute a
result and then have no way to tell you what it is. The door is not an addition to
user mode. Without it, user mode is pointless.

## Rule 1: the boundary is crossed by asking, not by jumping

The obvious design is: let the program call the kernel's function. Export
`print_string`, let the program call it.

It cannot, and the reason is worth getting right, because the intuitive reason is
not the real one.

The intuitive reason is that the kernel's address is secret. It is not, particularly,
and secrecy would be a terrible foundation anyway.

The first real reason: **the kernel's pages have the user bit clear.** A ring-3 call
to a kernel address faults on the instruction fetch. The program cannot even reach
the code, never mind run it.

The second real reason is the deeper one: **a call does not change privilege.**
Privilege lives in CS (chapter 9), and `call` does not touch CS. So even if the
program could reach the kernel's code, that code would execute at ring 3, and it
still could not do the privileged thing. You would have moved the problem, not
solved it.

So the crossing has to be something that *raises* privilege. And chapter 10
established there are only two things that do: interrupts and faults.

## Rule 2: so a syscall is a deliberate interrupt

The program interrupts itself. On purpose.

```
int 0x50
```

And everything chapter 3 built happens, unchanged. The CPU switches to the ring-0
stack it finds in the TSS (chapter 9's `rsp0`, finally doing its job), pushes the
interrupt frame, and vectors through the IDT to a stub. The stub pushes a dummy
error code, saves all fifteen general-purpose registers, and calls a C handler with
a pointer to the saved frame.

That is the exact same shape as a keyboard interrupt. The syscall path adds no new
stack layout, no new register-saving code, no new anything. The interrupt machinery
was already a general mechanism for "stop, raise privilege, run kernel code with the
old state saved, resume," and a system call is that mechanism used voluntarily.

This is the best kind of design payoff: the feature is nearly free because the
groundwork was general.

## Rule 3: the DPL on the gate is the door

Here is the mechanism that makes "one narrow doorway" a concrete thing rather than
a metaphor.

Every entry in the interrupt table carries a **DPL**, and it means: the lowest
privilege level allowed to reach this gate by executing `int N` deliberately.

In TownOS, every gate is DPL 0 except one. Vector 0x50 is DPL 3.

So:

- A program runs `int 0x50`. The gate allows ring 3. It goes through.
- A program runs `int 0x0E`, trying to fake a page fault and confuse the kernel into
  thinking something happened that did not. The gate is DPL 0. **General protection
  fault.**
- A program runs `int 0x40`, trying to fake a timer tick and get itself scheduled
  more often. Same. Denied.

**There is exactly one door, and the DPL is what makes it the only one.** Not
convention, not the kernel checking, not the program behaving. The hardware refuses
every other vector.

There is a nice subtlety in that. Hardware interrupts and genuine CPU exceptions
**ignore** the DPL entirely: a real page fault from ring 3 arrives fine through a
DPL 0 gate. The DPL governs only deliberate software `int` instructions. So making
every other gate DPL 0 costs nothing at all and buys the whole property.

## Rule 4: masked while inside

The syscall gate is an *interrupt* gate rather than a trap gate, which means the CPU
clears the interrupt flag on entry.

So a system call runs with interrupts off. It cannot be interrupted by the timer, it
cannot be interrupted by the keyboard, and it cannot be nested by another system
call.

That is a real simplification and a fair amount of TownOS quietly depends on it. In
particular it is why one shared ring-0 stack works: only one task is ever inside the
kernel at a time, so there is only ever one set of kernel C frames in existence.

It also sets the bill that chapter 19 has to pay. Because the kernel stack is
shared, a task cannot be parked halfway through a system call, and blocking has to
be built a completely different way.

## Rule 5: arguments go in registers, and that is not an accident

| Register | Role |
|---|---|
| RAX | syscall number in, return value out |
| RDI | first argument |
| RSI | second argument |
| RDX | third argument |

Why registers rather than a struct in memory that the program fills in and passes a
pointer to?

Because a pointer would itself have to be validated, and everything it pointed at
would have to be validated, and you would have added a checking problem to solve the
problem of passing arguments. Registers cross the boundary **by value**. There is
nothing to check about the number 42 arriving in RDI. It is just there.

That principle recurs: whenever you can move a value instead of a reference across a
trust boundary, do.

One related detail worth noticing. `include/syscalls.h` contains numbers and nothing
else. No types, no declarations, no includes. That is deliberate: a ring-3 program
has to know the numbers, and it must be able to learn them without including a
kernel header, because kernel pages are not user-readable and a ring-3 program that
tried to use anything declared in one would fault.

## Rule 6: the confused deputy

This is the heart of the chapter.

Consider `SYS_WRITE`. The program passes a pointer in RDI and the kernel prints the
string at that address.

Now think about who is running when that pointer is dereferenced. The **kernel** is,
at ring 0, where every page is readable.

So a program that cannot read kernel memory hands the kernel a kernel address and
says "please print this." The kernel can read it. The kernel prints it. The program
has just read memory it was categorically denied, and it did so entirely legally,
using the kernel as its hands.

That failure has a name. The kernel became a **confused deputy**: an agent with
authority, tricked into using that authority on behalf of someone who does not have
it.

Note what makes this possible, because it is structural rather than a slip. The
kernel is more privileged than its caller *and* is doing work on the caller's
behalf. That combination is the setup for the bug, and it is the permanent condition
of every system call that has ever existed.

So the rule: **every pointer that crosses the boundary is untrusted and must be
checked before it is followed.** Not the data it points at. The pointer itself.

## Rule 7: checking is harder than it looks

TownOS checks that a pointer lies inside the user region. That is the right shape.
Here is why the details are unforgiving.

**You have to check the whole range, not the start.** A pointer one byte below the
top of the user region, with a length of a megabyte, passes a start-only check and
then reads a megabyte of kernel memory. `SYS_WRITE` today has exactly this weakness,
and it is honest about it in a `TODO`: it validates the pointer and then calls
`print_string`, which walks until it finds a NUL, and there is nothing stopping that
walk from leaving the region.

**A string has no length until you have read it.** So you cannot check the range up
front. `copy_user_string` handles this the only way it can be handled: copy one byte
at a time, check the address against the region edge before each one, and refuse if
you reach the edge without finding a terminator. Slow, and correct.

**And you should copy it in rather than use it in place.** The check happens, and
then the use happens, and if anything could change the memory in between, the check
was worthless. That gap has a name too, **time of check to time of use**, and it is
not exploitable in TownOS today because syscalls run with interrupts off on a single
CPU. It is exploitable in essentially every real kernel, which is why they all copy.

## Rule 8: the number is untrusted too

Easy to forget. The program puts the syscall number in RAX, and the program can put
*anything* in RAX.

So the dispatcher must not use it as an index into a table of function pointers
without a bounds check, or a program passing 9999 gets the kernel to call whatever
happens to be at that offset. TownOS switches on it, and has a default case that
rejects unknown numbers.

**Switch on it, do not index with it.** Or if you index, check first.

## Rule 9: narrow on purpose

TownOS has ten system calls. Linux has around four hundred.

Every call is a permanent hole in the wall. Once a program can invoke it, that call
has to be correct about every argument any program will ever pass, including
arguments chosen specifically to break it, forever. A meaningful fraction of the
security history of every operating system lives in the fine print of individual
system calls: a length that was checked as signed, a pointer validated in one path
and not another, a flag combination nobody tested.

The cheapest system call to get right is the one you did not add. That is the actual
reason to keep the list short, and it is a better reason than tidiness.

## A trap you will meet two chapters early

Worth flagging here because it lives in this file's code.

Chapter 19 makes some system calls **blocking**, and it implements that by winding
the saved instruction pointer back onto the `int 0x50` so the woken task re-issues
the whole call from the top.

Which means that when the rewound `int 0x50` executes again, the CPU reads the
syscall number out of RAX **again**. So on a blocking path, the handler must not
write a return value into RAX. Do it and the woken task issues a different system
call than the one it asked for. Write zero there and it issues `SYS_EXIT` and kills
itself.

A return value that corrupts the *next* call rather than the current one. Chapter 19
explains why the design is this way. This is just where the sharp edge lives.

## What this still is not

- **No error numbers.** Almost everything returns -1 on failure, so a caller knows
  that something went wrong and nothing about what. `read: cannot read X` currently
  means the file is absent, or too large, or the disk failed, and the shell cannot
  tell which.
- **No permissions.** Any program may invoke any call. There is no notion of a
  program being allowed to read files but not start them.
- **Region-wide pointer checks, not per-process ones.** The check asks "is this in
  the user region", not "is this in *your* pages". Once each task has its own
  address space (chapter 14), the right check becomes narrower, and the code says so
  in a `TODO`.
- **No time.** There is no way for a program to ask what time it is, or to sleep for
  a duration. The timer ticks, but nothing exposes it.

## Exercises

1. A program cannot call a kernel function. Give both reasons, and say which one
   would still stop it if the other were fixed.
2. Every IDT gate is DPL 0 except vector 0x50. Describe what happens when a ring-3
   program executes `int 0x0E`, and then explain why a *genuine* page fault from
   ring 3 still works.
3. Design the confused-deputy attack against `SYS_WRITE` as it exists today. Give
   the pointer you would pass and say what you would expect to see.
4. `copy_user_string` copies a byte at a time and checks the address before each
   one. Why can it not check the whole range first and then copy in one go?
5. Syscall arguments are passed in registers rather than in a struct. Explain the
   security argument for that, not the performance one.
6. A dispatcher indexes a function-pointer table with the number in RAX, with no
   bounds check. Describe what a program would do with that, and what it would gain.
7. Time of check to time of use is not exploitable in TownOS today. Name the two
   properties that make it safe, and say which one disappears first as the kernel
   grows.
8. Syscalls run with interrupts off. Name two things elsewhere in the kernel that
   are simpler because of that, and one thing that is harder.
