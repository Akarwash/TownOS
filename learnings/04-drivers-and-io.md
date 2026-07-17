# 4. Device Drivers and I/O

**Source files:** `drivers/ports.c`, `drivers/screen.c`, `drivers/keyboard.c`,
`kernel/timer.c`

> **Note: the IRQ vector numbers below are out of date.** This chapter says the
> keyboard is on interrupt 33 and the timer on 32. MiniOS now uses a
> self-describing vector map, so hardware IRQs arrive at 0x40–0x4F: the timer is
> `IRQ_TIMER` (0x40) and the keyboard `IRQ_KEYBOARD` (0x41), both from
> `include/vectors.h`. The drivers register against those names, not raw numbers.
> See [../docs/decisions/0005-self-describing-vector-map.md](../docs/decisions/0005-self-describing-vector-map.md).
> The concepts below are unchanged; only the numbers moved.

A **device driver** is just code that knows the private language of one piece of
hardware. This chapter covers how software physically talks to devices, then walks
through MiniOS's three drivers: the screen (output), the keyboard (input), and the
timer (time).

## The big idea

Every driver, no matter how complex, is built from two primitives:

1. **A way to move bytes to and from the device** — either through special I/O
   ports or through memory-mapped registers.
2. **A way to be notified when the device has something to say** — an interrupt
   (chapter 3), or, failing that, polling.

That is genuinely all a driver is: *move bytes, react to events.* A modern GPU
driver and MiniOS's keyboard driver differ in scale, not in kind.

## Two ways to talk to hardware

### Port-mapped I/O (`drivers/ports.c`)

x86 has a *separate address space* just for devices, accessed with two dedicated
instructions: `in` (read from a port) and `out` (write to a port). A "port" is
just a 16-bit number identifying a device register. These instructions can only be
issued from privileged code (ring 0), which is another reason drivers live in the
kernel.

C cannot emit `in`/`out` directly, so MiniOS wraps them in inline assembly:

```c
uint8_t port_byte_in(uint16_t port) {
    uint8_t result;
    __asm__("in %%dx, %%al" : "=a"(result) : "d"(port));
    return result;
}
void port_byte_out(uint16_t port, uint8_t data) {
    __asm__("out %%al, %%dx" : : "a"(data), "d"(port));
}
```

The constraints (`"a"`, `"d"`) tell the compiler to put `port` in register `DX`
and the data in `AL`, because that is what the `in`/`out` instructions require.
These two tiny functions are the foundation of *every* MiniOS driver — the
keyboard, timer, and screen cursor all reach hardware through them. Notice how
much leverage a 4-line function provides: it is the sole gateway between C and the
device world.

### Memory-mapped I/O

The other approach: the hardware's registers or buffers appear at fixed *memory*
addresses, and you talk to the device by reading/writing memory normally. The VGA
text screen works this way — writing to address `0xB8000` puts characters on
screen (below). Modern hardware overwhelmingly uses memory-mapped I/O because it
is faster and not limited to 65,536 ports.

MiniOS uses **both**: memory-mapped for the screen buffer, port-mapped for the
cursor, keyboard, PIC, and timer. Good to see them side by side.

## Driver 1: the screen (`drivers/screen.c`)

The VGA text buffer lives at physical address `0xB8000`. It is an 80×25 grid where
each cell is **two bytes**: one ASCII character and one **attribute byte**
(foreground/background color). `WHITE_ON_BLACK` is `0x0F`. So the character at
row `r`, column `c` is at byte offset `(r * 80 + c) * 2`, and its color is the
next byte.

Writing text is literally writing bytes into that memory:

```c
video[offset * 2]     = c;              // the character
video[offset * 2 + 1] = WHITE_ON_BLACK; // its color
```

Two things make the screen a *driver* and not just an array:

**The cursor** is a hardware feature of the VGA controller, not part of the text
buffer. To move the blinking cursor you write its position to VGA registers 14 and
15 through the port pair `0x3D4`/`0x3D5` — you pick a register by writing its
number to `0x3D4`, then read/write the value at `0x3D5`. This is a classic
"index/data port" pattern you will meet again and again in hardware. That is why
`screen.c` uses *both* memory-mapped I/O (the buffer) and port I/O (the cursor).

**Scrolling** is pure software: when text reaches the bottom row, `scroll()`
`memcpy`s rows 1–24 up over rows 0–23 and blanks the last row. The hardware has no
idea it "scrolled"; the driver just rearranged bytes. A subtle, common bug (noted
in the build spec) is copying in the wrong direction and smearing the screen —
source must be row 1, destination row 0.

## Driver 2: the keyboard (`drivers/keyboard.c`)

The keyboard is the mirror image of the screen: input instead of output, and
interrupt-driven instead of write-on-demand.

When you press or release a key, the PS/2 keyboard controller raises **IRQ 1**
(remapped to interrupt 33 — chapter 3) and places a **scan code** at port `0x60`.
A scan code is *not* ASCII; it is a hardware key number. Pressing `A` gives scan
code `0x1E`; *releasing* `A` gives `0x1E | 0x80` = `0x9E`. The high bit
distinguishes press ("make") from release ("break").

The driver's job:

```c
void keyboard_callback(registers_t *regs) {
    uint8_t sc = port_byte_in(0x60);
    if (sc & 0x80) return;          // key release — ignore
    char c = scancode_to_ascii[sc]; // translate via lookup table
    if (c) shell_handle_keypress(c);
}
```

The `scancode_to_ascii` table is a 128-entry array mapping scan code → ASCII, with
`0` for keys MiniOS ignores. This translation table *is* the "US QWERTY layout" —
swap the table and you have a different keyboard layout, which is exactly how real
OSes implement keymaps.

Notice the layering that chapter 3 set up: `keyboard_init()` only has to call
`register_interrupt_handler(33, keyboard_callback)`. It never touches the IDT or
assembly. The interrupt plumbing and the driver are cleanly separated, and the
driver hands finished characters to the shell — one more layer up. Input flows
*up* the stack: hardware → PIC → CPU → stub → `keyboard_callback` → `shell`.

### A note on buffering

MiniOS passes each character straight to the shell. Real OSes interpose a **line
discipline**: keystrokes go into a buffer, backspace edits the buffer, and the
line is only delivered to the program on Enter. MiniOS's shell does a simplified
version of this itself (chapter 6). The general lesson: input often needs a
staging buffer between the raw device and the consumer.

## Driver 3: the timer (`kernel/timer.c`)

An OS needs a sense of time — to measure durations, to schedule tasks, to
eventually preempt them. That heartbeat comes from the **Programmable Interval
Timer (PIT)**, an old chip that can be configured to fire IRQ 0 at a chosen
frequency.

The PIT runs at a fixed base frequency of **1,193,180 Hz**. You program it by
loading a **divisor**: it fires at `base / divisor`. For a 100 Hz tick (one
interrupt every 10 ms):

```c
uint32_t divisor = 1193180 / frequency;   // frequency = 100
port_byte_out(0x43, 0x36);                 // command: channel 0, mode 3 (square wave)
port_byte_out(0x40, divisor & 0xFF);       // low byte of divisor
port_byte_out(0x40, (divisor >> 8) & 0xFF);// high byte of divisor
```

Then the driver registers a handler on interrupt 32 that just increments a
counter:

```c
static volatile uint32_t tick = 0;
static void timer_callback(registers_t *r) { tick++; }
```

That `tick` counter is MiniOS's entire concept of time. At 100 Hz, `tick / 100`
is seconds since boot. The shell's `tick` command reads it. `volatile` matters:
the variable changes behind the compiler's back (inside an interrupt), so the
compiler must not cache it in a register or optimise away reads.

This tiny driver is the seed of something huge. The moment you make
`timer_callback` do more than `tick++` — say, switch to a different task's saved
registers — you have a **preemptive scheduler**, and MiniOS becomes multitasking.
The timer interrupt is where "cooperative" OSes become "preemptive" ones.

## The shape shared by all three drivers

| | Screen | Keyboard | Timer |
|---|--------|----------|-------|
| Direction | output | input | input (events) |
| Transport | memory-mapped (`0xB8000`) + ports | port `0x60` | ports `0x40`/`0x43` |
| Notification | none (write on demand) | IRQ 1 → int 33 | IRQ 0 → int 32 |
| Core data | 80×25 cell buffer | scancode→ASCII table | tick counter |

Different devices, same skeleton: *a transport for bytes, and (for inputs) an
interrupt that runs a callback.* Once you have internalised this table, writing a
driver for a new device is mostly a matter of looking up its ports and its
protocol.

## Going further

- **Real drivers** deal with concurrency (a device and the CPU touching the same
  buffer), DMA (letting devices read/write RAM directly without the CPU copying
  each byte), power management, and hotplug. MiniOS ignores all of this — its
  devices are simple and always present.
- **The driver model**: Linux exposes a uniform interface (`open`/`read`/`write`/
  `ioctl`) so that *every* device looks like a file. That is job #4 from chapter 0
  — a uniform interface — realised. MiniOS calls its drivers directly instead.
- **Framebuffers**: real graphics replace the 80×25 text buffer with a
  memory-mapped array of pixels. The mechanism (write memory → see it on screen) is
  identical to `0xB8000`; only the resolution and format change.

### Exercises

1. Change `WHITE_ON_BLACK` to display green text. What does the attribute byte's
   layout have to be? (Look up VGA color attribute bits.)
2. The keyboard driver ignores key *releases*. What information are you throwing
   away, and which feature (hint: holding two keys) would need it?
3. Compute the divisor for a 1000 Hz timer. What is the trade-off between a high
   and low tick frequency for a future scheduler?
4. Both the cursor (`0x3D4`/`0x3D5`) and the PIC use an "index port + data port"
   pattern. Why is this pattern so common in hardware with many internal registers?
