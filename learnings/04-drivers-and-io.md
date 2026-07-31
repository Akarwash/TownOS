# 4. Device Drivers and I/O

**Source files:** `drivers/ports.c`, `drivers/screen.c`, `drivers/keyboard.c`,
`kernel/timer.c`

A **device driver** is just code that knows the private language of one piece of
hardware. This chapter covers how software physically talks to devices, then walks
through TownOS's three drivers: the screen (output), the keyboard (input), and the
timer (time).

## The big idea

Every driver, no matter how complex, is built from two primitives:

1. **A way to move bytes to and from the device**, either through special I/O ports
   or through memory-mapped registers.
2. **A way to be notified when the device has something to say**, which means an
   interrupt (chapter 3), or, failing that, polling.

That is genuinely all a driver is: *move bytes, react to events.* A modern GPU
driver and TownOS's keyboard driver differ in scale, not in kind.

## Two ways to talk to hardware

### Port-mapped I/O (`drivers/ports.c`)

x86 has a *separate address space* just for devices, accessed with two dedicated
instructions, `in` and `out`. Port 0x60 is not memory address 0x60. They are
different worlds that happen to use the same numbers.

`drivers/ports.c` wraps those two instructions in C functions, and every other
driver in the tree is built on top of them.

### Memory-mapped I/O

The other approach: the device's registers are wired into the normal memory
address space. Writing to a particular address writes to the device. No special
instructions, just a pointer.

The VGA text buffer at 0xB8000 is this. You write bytes into what looks like
ordinary memory and characters appear on screen, because those addresses are not
RAM, they are the video card.

## Driver 1: the screen (`drivers/screen.c`)

Memory-mapped, and the simplest driver in the tree. 0xB8000 holds 80x25 cells of
two bytes each: one character, one attribute byte for colour. Writing a cell puts a
character on screen immediately. There is nothing to wait for and no interrupt to
handle.

The one part that talks to ports rather than memory is the hardware cursor, which
lives behind an index-port/data-port pair at 0x3D4 and 0x3D5.

**Scrolling** is pure software: when text reaches the bottom row, `scroll()`
`memcpy`s rows 1 to 24 up over rows 0 to 23 and blanks the last row. The hardware
has no idea it "scrolled", the driver just rearranged bytes. A subtle, common bug
is copying in the wrong direction and smearing the screen. Source must be row 1,
destination row 0.

## Driver 2: the keyboard (`drivers/keyboard.c`)

The keyboard is the mirror image of the screen: input instead of output, and
interrupt-driven instead of write-on-demand.

Press or release a key and the PS/2 keyboard controller raises **IRQ 1** and places
a **scancode** at port 0x60. TownOS uses a self-describing vector map (chapter 3,
and `docs/decisions/0005-self-describing-vector-map.md`), so hardware IRQs arrive at
0x40 through 0x4F. The keyboard is `IRQ_KEYBOARD`, which is 0x41, from
`include/vectors.h`. The driver registers against that name, never a raw number.

A scancode is *not* ASCII. It is a hardware key number. Pressing `A` gives 0x1E.
*Releasing* `A` gives `0x1E | 0x80`, which is 0x9E. The high bit distinguishes a
press ("make") from a release ("break").

### The thing the hardware does not tell you

Here is the part that turns a lookup table into a small state machine.

**The keyboard has no concept of a capital letter.** It has a concept of a key
going down and a key coming up. Shift is a key like any other, reported twice: 0x2A
on the way down, 0xAA on the way up.

So "shift is held" is not information that exists in any single byte the hardware
hands you. It is a fact assembled from two events that can be an arbitrary number
of other keypresses apart, and somebody has to remember it in between. That
somebody is the driver.

Caps lock is stranger still. It reports 0x3A down and 0xBA up, exactly like any
other key. The hardware has no caps state at all. **The lock is entirely a fiction
the driver invents and maintains.**

So the driver keeps two flags, `shift_held` and `caps_on`, and holds two
translation tables instead of one: `scancode_to_ascii` and
`scancode_to_ascii_shift`, laid out identically, differing only in what each key
produces.

### How the two combine

Shift applies to every key. Caps lock applies to letters only.

And they combine with XOR, not OR. **Shift does not mean upper case, it means the
other case.** With caps lock on, shift+`a` gives you a lowercase `a`, which is what
every real keyboard does and what your fingers already expect. OR would give you
`A` and would also make caps lock shift the number row into `!@#$`.

So: a letter takes the shifted table when `shift_held != caps_on`. Everything else
takes it when `shift_held`, ignoring `caps_on` entirely. That is what `table_for()`
does.

### The trap worth remembering

A release of left shift arrives as **0xAA**, not 0x2A. If you check the raw byte
against 0x2A you never match, `shift_held` is set and never cleared, and from then
on every key is shifted. The entire keyboard looks broken and the cause is one
missing mask.

So the release branch masks bit 7 off *first*, then asks whether what came up was a
modifier:

```c
if (scancode & KEY_RELEASE_MASK) {
    uint8_t released = scancode & KEY_CODE_MASK;
    if (released == KEY_LSHIFT || released == KEY_RSHIFT) {
        shift_held = 0;
    }
    return;
}
```

Order matters as much as the mask. The modifier check has to happen *before* the
return, not after it.

### Where the character goes

The old version of this chapter said the driver hands finished characters straight
to the shell. That was true when the shell lived inside the kernel. It is not true
any more, and the reason it changed is a rule about interrupt handlers in general.

**An interrupt handler runs with interrupts disabled.** While it is running the
timer is not ticking, no other device is being serviced, and nothing else on the
machine makes progress. So the rule: do the least possible work and return.

Handling a keypress properly means echoing it, editing a line, maybe running a
whole command. That is a lot of work, and doing it inside the IRQ froze the machine
for the duration.

Now the handler decodes one scancode, pushes one character into a **ring buffer**,
and returns. Somebody else drains that buffer later, on their own time.

A ring buffer is a fixed array with a read index and a write index chasing each
other around it. One slot is deliberately left unused: if a full buffer were
allowed to wrap the write index onto the read index, "full" would look exactly like
"empty" and the buffer would silently discard everything.

Who drains it is not this chapter's business. It is a ring-3 program calling a
system call, which you have not met yet (chapters 13 and 18). The driver's job ends
the moment the character is in the buffer.

There is one more line in the handler, `scheduler_wake(WAIT_KEY)`, which exists
because a program that is asleep waiting for a key cannot notice one arriving. That
is chapter 19 and it will not make sense yet. What matters here is that modifier
presses return before reaching it: a shift press that woke every sleeping task
would have them all wake up, find an empty buffer, and go back to sleep, which is
exactly the waste that blocking was built to remove.

### The layering

Notice what chapter 3 bought you. `keyboard_init()` is one line:

```c
void keyboard_init(void) {
    register_interrupt_handler(IRQ_KEYBOARD, keyboard_callback);
}
```

It never touches the IDT and never touches assembly. Input flows up the stack:
hardware, then PIC, then CPU, then stub, then `keyboard_callback`, then the ring
buffer, and eventually out to a program. Each layer knows only about the one below
it.

### About the tables

The translation tables *are* the "US QWERTY layout". Swap them and you have a
different keyboard layout, which is exactly how real operating systems implement
keymaps. Note the plural: there are two now, and a layout change means swapping
both. Nothing in the code checks that the two tables agree with each other, which
is 256 hand-written entries defended only by the fact that they sit next to each
other in the file.

### What is still missing

**Extended scancodes.** Arrow keys, right ctrl and right alt send 0xE0 first and
the real code second. 0xE0 has bit 7 set, so the release branch swallows it and the
byte after it is decoded as an ordinary key. That is harmless today by luck rather
than design: the arrow codes fall past the populated part of the table and decode
to 0. Doing it properly needs a "saw 0xE0" flag and a third table, and it belongs
with whatever first needs an arrow key.

**Key releases, generally.** Only shift releases are acted on. Everything else is
dropped, which means the driver cannot tell you that a key is *still down*. Key
repeat, chords, and anything game-like need both edges.

**LEDs.** There is no way to see whether caps lock is on except by typing a letter.
Lighting the LED means writing a command *to* the keyboard and handling its
acknowledgement, and this driver only ever reads.

All three of these are consequences of one decision, recorded in
`docs/decisions/0019-keyboard-modifier-state-in-the-driver.md`: the buffer carries
resolved characters rather than raw scancodes. That decision is right for today,
where the only consumer is a shell that wants text, and it is the thing that will
have to change first when something wants more.

## Driver 3: the timer (`kernel/timer.c`)

An OS needs a sense of time, to measure durations, to schedule tasks, to preempt
them. That heartbeat comes from the **Programmable Interval Timer (PIT)**, an old
chip that can be configured to fire IRQ 0 at a chosen frequency.

The PIT runs at a fixed base frequency of **1,193,180 Hz**. You program it by
loading a **divisor**: it fires at `base / divisor`. For a 100 Hz tick, one
interrupt every 10 ms:

```c
uint32_t divisor = 1193180 / frequency;    // frequency = 100
port_byte_out(0x43, 0x36);                 // command: channel 0, mode 3 (square wave)
port_byte_out(0x40, divisor & 0xFF);       // low byte of divisor
port_byte_out(0x40, (divisor >> 8) & 0xFF);// high byte of divisor
```

Then the driver registers a handler on `IRQ_TIMER` (0x40) that increments a
counter:

```c
static volatile uint32_t tick = 0;
static void timer_callback(registers_t *r) { tick++; }
```

That `tick` counter is TownOS's entire concept of time. At 100 Hz, `tick / 100` is
seconds since boot. `volatile` matters: the variable changes behind the compiler's
back, inside an interrupt, so the compiler must not cache it in a register or
optimise away reads of it.

This tiny driver turned out to be the seed of something much larger. The moment
`timer_callback` does more than `tick++`, say switch to a different task's saved
registers, you have a **preemptive scheduler**. That is exactly what happened in
chapter 12. The timer interrupt is where cooperative operating systems become
preemptive ones.

## The shape shared by all three drivers

| | Screen | Keyboard | Timer |
|---|--------|----------|-------|
| Direction | output | input | input (events) |
| Transport | memory-mapped (0xB8000) + ports | port 0x60 | ports 0x40 / 0x43 |
| Notification | none (write on demand) | IRQ 1, `IRQ_KEYBOARD` (0x41) | IRQ 0, `IRQ_TIMER` (0x40) |
| Core data | 80x25 cell buffer | two scancode tables, two modifier flags, a ring buffer | tick counter |
| Handler does | everything, inline | decode one key, push, return | `tick++` |

Different devices, same skeleton: *a transport for bytes, and, for inputs, an
interrupt that runs a callback.* Once you have internalised this table, writing a
driver for a new device is mostly a matter of looking up its ports and its
protocol.

Note the bottom row, because it is the lesson that took longest to learn here. The
screen driver can do all its work inline because nothing interrupts it. The two
interrupt-driven drivers both do almost nothing and hand off.

## Going further

- **Real drivers** deal with concurrency (a device and the CPU touching the same
  buffer), DMA (letting devices read and write RAM directly without the CPU copying
  each byte), power management, and hotplug. TownOS ignores all of this. Its
  devices are simple and always present.
- **The driver model**: Linux exposes a uniform interface (`open`, `read`, `write`,
  `ioctl`) so that *every* device looks like a file. That is job number 4 from
  chapter 0, a uniform interface, realised. TownOS calls its drivers directly
  instead.
- **Framebuffers**: real graphics replace the 80x25 text buffer with a
  memory-mapped array of pixels. The mechanism (write memory, see it on screen) is
  identical to 0xB8000. Only the resolution and the format change.

### Exercises

1. Change `WHITE_ON_BLACK` to display green text. What does the attribute byte's
   layout have to be? (Look up the VGA colour attribute bits.)
2. Caps lock is on. You press shift and `a`. Trace every byte the driver receives
   and every flag it touches, and say which table each lookup uses.
3. Someone "simplifies" the release branch to `if (scancode & KEY_RELEASE_MASK)
   return;`. Describe exactly what the keyboard does from then on, and how long it
   takes a user to notice.
4. The ring buffer deliberately wastes one slot. Describe what a user would see if
   it did not, and why the symptom would be so confusing.
5. Compute the divisor for a 1000 Hz timer. What is the trade-off between a high
   and a low tick frequency for a scheduler?
6. Both the cursor (0x3D4 / 0x3D5) and the PIC use an "index port plus data port"
   pattern. Why is this pattern so common in hardware with many internal registers?
7. The driver resolves scancodes to characters before they reach the buffer. Name
   three features that decision makes impossible, and say what would have to change
   in `SYS_READKEY` to allow them.
