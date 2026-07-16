# Prologue: There Is No Supervisor

**Read this before chapter 0. It is not about MiniOS. It is not about x86. It is
the one idea that every other chapter turns out to be a consequence of.**

If you have read the other chapters and feel like you are holding seven unrelated
topics at once, that feeling is correct and this document is the fix. There are
not seven topics. There is one idea and seven angles on it.

## Forget everything first

Forget operating systems. Forget the GDT, the IDT, the PIC, paging, drivers.
Forget MiniOS entirely. We are starting from before all of it.

## What a CPU is

A CPU is this program, burned into silicon:

```
forever:
    instruction = memory[EIP]
    EIP = EIP + length(instruction)
    execute(instruction)
```

`EIP` is a register. It holds an address. That is it. That is a CPU.

It does not supervise anything. It does not know what a program is. It does not
know what a file is, or a process, or an operating system. It reads a number from
an address, does what that number says, moves to the next address, and repeats. It
has been doing this since the power supply asserted `POWER_GOOD`, and it will do it
until you cut power.

Everything below follows from this and nothing else.

## There is exactly one EIP

One instruction pointer. One.

So: you have a browser open, music playing, a terminal, and a chat app. Four
programs "running at the same time."

**How many are actually running?**

One. At any given nanosecond, exactly one. Whatever `EIP` points at right now. That
is the definition of running. Nothing else is executing, because there is nothing
else to execute *with*.

The other three are piles of bytes sitting in RAM, completely inert, with their
register values written down somewhere. They are not "paused." They are not
"waiting." They are not doing anything at all. They are data.

## So where is the operating system?

Here is the question worth actually sitting with.

The browser is running. `EIP` is somewhere inside the browser's code. Where is the
kernel?

Not "what is the kernel doing." **Where is it.**

Answer: it is a pile of bytes in RAM. Inert. Not executing. Not watching. Not
supervising. Not checking on anything. The kernel is **not a process**. It does not
have its own `EIP`. It cannot, because there is only one and the browser is using
it.

This is the sentence that unlocks the subject, and it is the opposite of what
almost everyone assumes:

> **While your program runs, the operating system is not running.**

There is no supervisor. There is no watcher. There is no little kernel daemon
spinning in the background making sure everything is okay. That thing does not
exist and never has.

## Then how does the OS ever run again?

Now the puzzle. The browser is executing. Suppose it does this:

```c
while (1) { }
```

An infinite loop. `EIP` will bounce between two addresses inside the browser
forever. It will never call anything. It will never return. It has no interest in
giving the CPU back and no reason to.

**What takes the CPU away from it?**

Whatever answer comes to mind, check it against this: any *software* that could
stop the browser would itself need the CPU in order to execute. And the CPU is
executing the browser. Software cannot interrupt software. There is nothing to run
the interrupting code *on*.

So the answer cannot be software. It has to come from outside the fetch-execute
loop entirely.

## The answer is a wire

There is a physical pin on the CPU package. A wire. When a voltage appears on it,
the silicon does something the fetch-execute loop above does not describe.

It finishes the current instruction. Then, instead of `EIP = EIP + length`, it:

1. Writes the current `EIP` onto the stack.
2. Looks up an address in a table in memory.
3. Sets `EIP` to that address.

And then the loop continues exactly as before, fetching and executing. It just does
it somewhere else now.

The browser did not agree to this. The browser was not asked. The browser does not
know it happened. Some hardware pulled a wire and the instruction pointer got
yanked out of the browser and dropped into the kernel.

**That is what an interrupt is.** Not a feature. Not a notification. A wire that
moves `EIP`.

And a timer chip is wired to that pin. It pulls it a hundred times a second,
whether anyone wants it to or not.

That is the entire mechanism by which operating systems exist. Without it, the
first program to run would run forever, and there would be no such thing as an OS.

## The picture

```
                        timer chip
                             |
                             |  raises the wire
                             v
        +--------------------|-------------------------------+
        |  user program      |                               |
        |                    |                               |
        |   o----------------+                 .-----------> |
        |                    |                 |             |
        +--------------------|-----------------|-------------+
                             |                 |
                  interrupt  |                 |  iret
                             |                 |
        +--------------------|-----------------|-------------+
        |  kernel code       |                 |             |
        |                    '-----------------'             |
        |                                                    |
        +----------------------------------------------------+

           the single line is EIP. there is only one of it.
```

The line never branches and never doubles. When it is down in the kernel, the user
program is not "slowed down" or "deprioritised." It has stopped existing as a
running thing. It is data again.

## Every chapter is a consequence of the wire

Each of the seven chapters answers one question that the wire forces you to ask.
None of them are independent topics.

| The wire forces this question | The answer | Chapter |
|---|---|---|
| The CPU looks up an address in a table. Which table? What format? | The **IDT** | 3 |
| Many devices want the one pin. Who arbitrates, and how does the CPU know who called? | The **PIC** | 3 |
| `EIP` landed in your code with zero warning. Every register still holds the interrupted program's values. | `pusha` in the **stubs** | 3 |
| When you are done, `EIP` and the flags must go back exactly, so the program never knows. | **`iret`** | 3 |
| While a program runs, the kernel's bytes sit in RAM defenceless. If they get overwritten, the wire fires into garbage. | **Rings, GDT, paging** | 2, 5 |
| When there is nothing to do, do not spin. Stop the CPU and let the wire restart it. | **`hlt`** | 6 |
| Devices other than the timer also pull the wire. What is the code they land in? | **Drivers** | 4 |
| Who set `EIP` the very first time? | **Boot** | 1 |

Seven chapters. One idea. Read the table again after you finish chapter 7 and it
will read as obvious.

## Where MiniOS sits in this

MiniOS has no user programs. There is nothing else in RAM.

So when the timer wire fires, what is it interrupting?

**The kernel's own `hlt` loop.** That is all. `EIP` gets yanked out of
`while (1) hlt;`, dropped into `timer_callback`, `tick++`, and returned. Ten
milliseconds later, again.

When you press a key, `EIP` gets yanked out of that same `hlt` loop, dropped into
`keyboard_callback`, which calls the shell, which runs the whole command, and then
returns.

This is why chapter 6 says the shell is not really a program but a set of callbacks
hanging off the keyboard interrupt. The shell has never had its own `EIP`. Nothing
in MiniOS does. There is one instruction pointer, it sleeps in a `hlt` loop, and
wires wake it up.

That is the whole operating system. It really is that.

## The four sentences worth memorising

You will not remember `0xB8000`, the ICW1 through ICW4 sequence, or the PIT command
byte layout. Nobody does. Everyone looks those up. What you are supposed to retain
is this:

1. A table describes rules, a register points at the table, and a special
   instruction commits it. The GDT, the IDT, page tables, and the IOMMU are all
   this same pattern.
2. An interrupt is a forced diversion of `EIP` that returns exactly where it left
   off. Everything reactive is built on it.
3. A driver moves bytes and reacts to events. That is all a driver is.
4. Hardware protection is *authorisation*, not *validation*. It checks who is
   asking, never what they are asking for. Its failure mode is an interrupt.

Everything else is lookup. That is the ground you stand on.

## Seeing it for yourself

Theory sticks once you watch it happen. These three commands turn the emulator from
a black box into an instrument, and no other chapter mentions them.

**Log every interrupt as it fires:**

```
qemu-system-i386 -kernel minios.bin -d int -no-reboot -no-shutdown
```

`-d int` prints every vector with its error code, `EIP`, `CS`, and `EFLAGS`.
`-no-reboot -no-shutdown` stops the machine resetting on a triple fault, so you can
read the last three faults instead of watching a reboot loop. A triple fault stops
being a mystery and becomes a line saying `v=0e e=0002`, which is a page fault, on
a write, to a not-present page.

**Attach a real debugger:**

```
qemu-system-i386 -kernel minios.bin -s -S
```

then, in another terminal:

```
i686-elf-gdb minios.bin
target remote :1234
break kernel_main
continue
```

Now you can single-step through `gdt_flush`, watch `CS` change across the far jump,
step into an ISR stub, and `p *regs` inside `irq_handler` to see the struct that the
assembly built.

**Inspect CPU state directly:**

```
qemu-system-i386 -kernel minios.bin -monitor stdio
```

At the `(qemu)` prompt, `info registers` prints `CS`, `EFLAGS`, and the real base
and limit of your GDT and IDT. This is how you verify a GDT that produces no visible
output. `xp/16xb 0xB8000` dumps the physical bytes of the VGA buffer so you can see
chapter 4's character and attribute pairs with your own eyes.

Do this before writing another line of kernel code. Chapter 1 claims that at
`kernel_main` the CPU is in 32-bit protected mode with interrupts disabled. Go
check. Look at `EFLAGS` and confirm bit 9 is clear.

That is what it feels like to not be lost. Not "I read that interrupts are
disabled." *I looked, and they were disabled.*

## Exercises

Answer these before reading chapter 0. Do not look anything up. The point is to see
the shape of what is in your head, not to be right.

1. A user program is running `while (1) { }`. The kernel is not executing. Nothing
   in the kernel is executing. Explain how the kernel ever runs again.
2. Suppose the kernel earlier executed `cli`, which clears a flag telling the CPU to
   obey the interrupt wire, and forgot to set it again. Same infinite loop. What
   happens to the machine, and why? (This is the important one. If you get it, you
   have the idea.)
3. Given exercise 2, why do you think `cli` is a privileged instruction that a ring
   3 program is not allowed to execute?
4. MiniOS runs `while (1) hlt;` instead of `while (1) {}`. Both loops "do nothing."
   Explain the difference in terms of what the CPU is physically doing, and why the
   difference only makes sense once interrupts exist.
5. The kernel is not running while a user program runs. So who saves the user
   program's registers before the kernel clobbers them, and at what exact moment?
