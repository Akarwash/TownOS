# Chapter 9: The 64-bit GDT and the TSS

> Read chapter 8 (long mode and paging) first. That chapter ended one instruction
> short of finishing the climb, waiting for a table. This is the table.

## Where we are

Chapter 8 got the CPU into long mode and then stopped, because the last step of the
climb is a far jump that reloads the code segment register, and a code segment
register can only be loaded with something that names an entry in a table.

That table is the **GDT**, the Global Descriptor Table. It is a leftover from an
architecture that no longer exists, it describes a memory model the CPU no longer
uses, and it is also the single thing that makes the boundary between your kernel
and a user program possible.

Both of those are true at once, and understanding why is the chapter.

## Rule 1: segmentation is dead, the table is not

Before paging, x86 divided memory using **segments**. A segment descriptor held a
base address and a limit, and every memory access was checked against them:
addresses were offsets from the base, and going past the limit was a fault. That was
how you kept programs apart.

Paging does that job better, so segmentation lost. In long mode the CPU **ignores
the base and limit** on code and data descriptors entirely. You can set them to
anything. The hardware does not look.

So the obvious question: why is the table still there?

Because the CPU still reads *other* fields out of it. The base and limit are
vestigial. The rest is load-bearing.

## Rule 2: what the hardware still insists on being told

Strip a descriptor down to what long mode actually uses and you are left with a
short list:

- **Present.** Does this entry mean anything.
- **DPL.** The privilege level, 0 to 3. This is the important one.
- **Executable.** Is this code or data.
- **L.** For a code segment, is it 64-bit.

That is close to the whole of it. A 64-bit descriptor is mostly a privilege level
wearing an eight-byte costume left over from 1985.

## Rule 3: privilege lives in CS

Here is the central idea, and it is not obvious.

The CPU does not have a "kernel mode" flag you set. There is no bit somewhere
called `am_i_the_kernel`. What it has is a **current code segment**, held in the CS
register, and that segment names a descriptor, and that descriptor carries a
privilege level.

So the privilege the machine is running at *is* a property of the table entry that
CS currently points at.

Change which descriptor CS names and you have changed privilege level. That is the
whole mechanism. There is nothing else to it.

Which is why this dead table is the thing that makes the kernel and user boundary
possible: **the boundary is a pair of entries in it.** One that says DPL 0, one that
says DPL 3. Running kernel code means CS points at the first. Running a program
means CS points at the second. Chapter 10 is entirely about how you get from one to
the other, and it only works because these two entries exist.

## Rule 4: so you need four, not one

Kernel code, kernel data, user code, user data. Two privilege levels, and each
needs a code descriptor and a data descriptor, because code and data descriptors
differ in the executable bit and the CPU checks it.

Plus a **null descriptor** at index 0, which describes nothing and is there
precisely so that a selector of 0 is unambiguously invalid. Load 0 into a segment
register and use it and you fault immediately, rather than accidentally getting
whatever descriptor happened to be first.

The number you load into a segment register is a **selector**, and it is not just an
index. Its low two bits carry the privilege level being requested. So the user code
descriptor at index 3 is selected with `0x18 | 3`, which is `0x1B`. That "| 3" is
the program declaring it is asking as ring 3, and it is a small thing that will
matter in chapter 10 when the kernel forges a stack frame to drop into user mode.

## Rule 5: two tables, in sequence

TownOS builds the GDT twice.

`boot/boot.asm` installs a **bootstrap GDT** with the bare minimum needed to make
the far jump legal. Then, once C is running, `gdt_init()` installs the **real
kernel GDT** with all seven slots.

Why not do it once? Because the bootstrap table has to exist in assembly, before C
exists, and the real table wants to be a C struct with a TSS embedded in it and
static assertions on its layout. Doing both in assembly would mean writing the
interesting parts twice in the harder language. Doing both in C is impossible
because the far jump has to happen before C runs.

There is one constraint that ties them together, and it is the kind of thing that
produces an unforgettable afternoon if you get it wrong: **the kernel code and data
selectors must mean the same thing in both tables.** 0x08 is kernel code in both,
0x10 is kernel data in both. The code that swaps the tables reloads CS partway
through, and if 0x08 meant one thing before the swap and something else after, that
reload lands on a descriptor for something entirely different and the machine dies
inside the function whose job was to make things work.

## Rule 6: the TSS, and what is left of it

The **TSS**, the Task State Segment, is the second leftover in this chapter and it
has been hollowed out even more thoroughly than segmentation.

In 32-bit x86 the TSS held an entire task's saved context, and the CPU could switch
between tasks in hardware by loading a different one. It was a genuine feature. It
was also slower than doing the same thing in software, so essentially nobody used
it, and in 64-bit mode it was removed.

What remains is a 104-byte structure that mostly holds stack pointers. And of those,
one field matters: **rsp0**.

## Rule 7: why rsp0 has to exist

This is the best question in the chapter, so work through it rather than reading the
answer.

A ring-3 program is running. It is using its own stack. An interrupt fires, or it
makes a system call. The CPU has to start running the kernel's handler, at ring 0.

Can the handler run on the program's stack?

No, and for two independent reasons. First, the program controls that stack pointer.
It could have set it to point at the kernel's own data, and then the CPU would
happily push the interrupt frame on top of something important, at the program's
choosing. That is not a bug, that is a way in. Second, the stack pointer might
simply be invalid, or unmapped, or zero, and now your interrupt handler faults
before it has run an instruction.

So the CPU must switch to a stack the *kernel* chose, before the handler runs.

Which raises the real question: **where does the CPU find that stack?**

It cannot ask the kernel. It is not running kernel code yet, that is the entire
problem. The address has to be somewhere the hardware already knows to look, put
there in advance.

That place is the TSS, and the field is `rsp0`. The kernel writes the top of a
ring-0 stack into it at boot, and from then on, every time the CPU crosses from ring
3 into ring 0, it reads that field and switches to that stack before pushing
anything.

**The TSS is a note the kernel leaves for the CPU about where to land.** That is
what it is for, and after chapter 10 it is essentially all it is for.

## Rule 8: built before it does anything

The TSS is inert right now. Everything runs at ring 0, so no interrupt crosses a
privilege boundary, so the CPU never reads `rsp0`. You could delete it today and
nothing would change.

It was built anyway, deliberately, and that decision is recorded in
`docs/decisions/0004-build-tss-before-user-mode.md`. The reasoning is worth
absorbing because it generalises: the alternative is discovering you need it at the
exact moment the first ring-3 program runs, which is the moment when nothing works,
nothing prints, and the failure is a reset with no message. Build the thing that
catches you *before* you jump.

## Rule 9: the descriptor that did not fit

A small, funny consequence of a fixed hardware format.

Code and data descriptors have a 32-bit base field. That is fine, because the base
is ignored. The TSS descriptor's base is *not* ignored: it is the actual linear
address of the TSS structure, and in a 64-bit system that address does not fit in
32 bits.

So the system descriptor was extended with another 32 bits of base and a reserved
word, making it 16 bytes instead of 8, and it therefore occupies **two slots** in
the table. That is why the GDT here has seven entries for six things.

You will meet this shape repeatedly in systems work: the format is fixed, the format
is too small, and the extension gets bolted onto the end rather than the format
being fixed properly, because fixing it would break everything that already exists.

## Rule 10: you cannot far jump in 64-bit mode

One more.

In 32-bit code you reload CS with `jmp 0x08:label`, a far jump with the selector and
the offset written directly into the instruction. That encoding **does not exist in
64-bit mode.** It was removed.

So reloading CS is done with a far *return* instead: push the selector, push the
address, and execute a return that pops both. You are faking a return from a
function you were never called by, in order to land at a chosen address with a
chosen privilege.

Remember that trick. Chapter 10 gets into user mode the same way, by forging a
frame for a return that never had a matching call.

## What this still is not

- **One TSS, one ring-0 stack.** Every task in the machine enters the kernel on the
  same stack, because there is one `rsp0` and it never changes. This is fine while
  only one task is ever inside the kernel at a time, which is true here. It is also
  the exact constraint that forces the design in chapter 19: with a single shared
  kernel stack, a task cannot be frozen halfway through a system call, because its
  C frames get overwritten by the next task to enter. One field in a leftover
  structure from 1985 decides how blocking works.
- **No I/O permission bitmap.** The TSS can carry a bitmap saying which I/O ports a
  ring-3 program may touch directly. TownOS sets the offset past the end of the
  structure, which means "none, ask the kernel."
- **No interrupt stack table.** The seven IST slots let specific interrupt vectors
  use their own dedicated stacks, which is how real kernels survive a fault that
  happens while the stack itself is broken. All seven are zero here.

## Exercises

1. The CPU has no "kernel mode" flag. Explain, in terms of CS and the GDT, what
   actually changes when the machine goes from running the kernel to running a
   program.
2. The user code selector is `0x1B`, not `0x18`. What are the low two bits for, and
   what would go wrong if a program loaded `0x18` instead?
3. Why is index 0 of the GDT a null descriptor rather than something useful?
   Describe the class of bug it catches.
4. The bootstrap GDT and the kernel GDT must agree on the meaning of `0x08`.
   Describe exactly what happens if they do not, and say which function the machine
   dies inside.
5. A ring-3 program sets its stack pointer to the middle of the kernel's task table
   and then triggers an interrupt. Describe what would happen with no `rsp0`, and
   what happens with it.
6. The TSS is inert until user mode exists. Argue both sides of building it early,
   and then say which side you find more convincing and why.
7. The TSS descriptor is 16 bytes because a 64-bit base does not fit in a 32-bit
   field. Name another place in this kernel where a structure was extended rather
   than redesigned, and say what the extension cost.
8. Chapter 19's blocking design is forced by there being a single `rsp0`. Sketch
   what would change in chapter 19 if each task had its own kernel stack.
