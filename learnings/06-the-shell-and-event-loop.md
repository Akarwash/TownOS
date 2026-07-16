# 6. The Shell and the Event Loop

**Source files:** `kernel/kernel.c`, `shell/shell.c`, `shell/shell.h`

This chapter ties everything together. You have a booted CPU (ch.1), described
memory (ch.2), working interrupts (ch.3), and drivers (ch.4). Now we assemble them
into a system that *does something*: reads commands and responds. Along the way we
answer the question that surprises most beginners — *what does an OS do when there
is nothing to do?*

## The big idea: an OS is an event loop that sleeps

A running kernel is not a program that marches from top to bottom and exits. It is
a program that **initialises**, then **waits for events forever**, doing work only
when an interrupt wakes it. That structure — *set up, then loop reacting to
events* — is the same shape as a GUI application's event loop, a game loop, or a
web server. The OS is just the most fundamental instance of it.

MiniOS's `kernel_main` makes this concrete in two halves:

```c
void kernel_main(void) {
    /* --- Half 1: initialisation (runs once) --- */
    gdt_init();              // ch.2: describe memory
    isr_install();           // ch.3: install interrupt handlers, enable interrupts
    timer_init(100);         // ch.4: start the 100 Hz heartbeat
    keyboard_init();         // ch.4: start listening for keys
    screen_clear();          // ch.4: blank the screen
    print_string("Welcome to MiniOS!\n");
    shell_init();            // print the first prompt

    /* --- Half 2: the idle loop (runs forever) --- */
    while (1) {
        __asm__ __volatile__("hlt");
    }
}
```

The order of half 1 is not arbitrary — it is a dependency chain. You cannot handle
interrupts before the GDT defines the code segment those handlers run in. You must
not `timer_init`/`keyboard_init` (which enable devices that raise IRQs) before
`isr_install` has set up handlers, or the first IRQ jumps into the void. **Describe
memory, install interrupt handling, then turn on the devices that use it.**

## What `hlt` really does (the surprising part)

That `while (1) hlt;` loop looks like it does nothing, and that is exactly right —
but *how* it does nothing matters enormously.

`hlt` is an x86 instruction that **stops the CPU until the next interrupt
arrives**. The processor genuinely powers down execution units and waits. When a
key is pressed or the timer fires, the interrupt wakes the CPU, the handler runs,
and then `iret` returns... to the instruction after `hlt`, whereupon the loop runs
`hlt` again and the CPU goes back to sleep.

Contrast with a busy-wait, `while (1) {}`, which pins the CPU at 100% spinning
uselessly — burning power, spinning up laptop fans, wasting a core. The `hlt` loop
uses ~0% CPU. This is why a real idle computer is cool and quiet: the OS's idle
task is doing essentially this. **The kernel spends the vast majority of its life
halted, woken only by interrupts.** Everything useful happens inside interrupt
handlers or the code they wake.

This is the moment chapter 3 pays off completely. Without interrupts, an idle loop
would be a dead machine. With them, an idle loop is a responsive OS. The two
concepts only make sense together.

## How a keypress becomes a command

Follow one command end-to-end. The control flow crosses every layer you have built:

```
1. You press 'l'.
2. Keyboard hardware raises IRQ 1 → PIC → CPU (ch.3).
3. CPU consults IDT entry 33 → runs irq1 stub → common stub → irq_handler.
4. irq_handler sends EOI, calls the registered keyboard_callback (ch.4).
5. keyboard_callback reads scancode 0x60, translates to 'l',
   calls shell_handle_keypress('l').
6. shell_handle_keypress appends 'l' to input_buffer and echoes it to screen.
7. ... repeat for 's', then Enter ...
8. On '\n', shell_handle_keypress null-terminates the buffer and calls
   shell_execute("ls").
9. shell_execute strcmp's "ls" against known commands, runs the match or
   prints an error, then prints a fresh "> " prompt.
10. CPU returns via iret to the hlt loop and sleeps until your next key.
```

Every chapter of these docs appears in that list. The shell is not really "a
program" in MiniOS — it is a set of callbacks hanging off the keyboard interrupt.
That is a legitimate architecture for a tiny single-tasking OS.

## Inside the shell (`shell/shell.c`)

The shell keeps two pieces of state:

```c
static char input_buffer[256];
static int  buffer_pos;
```

and exposes three functions:

- **`shell_init()`** — zero the buffer and print the prompt `"> "`.
- **`shell_handle_keypress(char c)`** — the per-character state machine:
  - `'\n'` (Enter): terminate the buffer, call `shell_execute`, reset, new prompt.
  - `'\b'` (Backspace): if the buffer is non-empty, drop the last char and erase it
    on screen (move cursor back, print space, move cursor back — three operations,
    a classic gotcha from the build spec).
  - otherwise: append `c` to the buffer and echo it.
- **`shell_execute(char *cmd)`** — compare `cmd` against known commands with
  `strcmp` and act:

| Command | Action |
|---------|--------|
| `help`  | list available commands |
| `clear` | `screen_clear()` then reprint prompt |
| `hello` | print a greeting |
| `tick`  | print the timer tick counter (ch.4) |
| *other* | `Unknown command: ...` |

This is a **command dispatcher** — the same pattern as a real shell's builtin
lookup, just with `strcmp` instead of a hash table. Adding a command is one more
`strcmp` branch. The `tick` command is a nice cross-cut: it reads state maintained
by an interrupt handler in a completely different file, demonstrating how the timer
driver and the shell communicate through shared kernel state.

### Why `int_to_string`

To print the tick count, the shell needs to turn a `uint32_t` into decimal digits
— there is no `printf` in a freestanding kernel (ch.7). So you write a small
`int_to_string`/`itoa` helper: repeatedly take `n % 10` for the last digit and
`n / 10` to shift down, collecting digits (in reverse, then flip them). It is a
five-line function, but writing it yourself is a good reminder of how much the
standard library normally does for you.

## Cooperative vs. preemptive: where MiniOS sits

MiniOS is **single-tasking**: there is exactly one thread of control (the shell),
plus interrupt handlers that briefly borrow the CPU and return. Nothing schedules
multiple programs.

The leap to **multitasking** is smaller than it looks, and you already have the
key ingredient — the timer interrupt (ch.4). A **preemptive scheduler** works like
this: on each timer tick, the handler saves the current task's `registers_t`,
picks another task, and restores *its* registers, so `iret` returns into a
different task. The timer becomes a "switch now" signal and the CPU is transparently
shared. That single change turns this event-loop kernel into a multitasking one,
and it is the most natural next project after MiniOS boots.

## Going further

- **A real shell** (bash, zsh) is a *user-space program*, not part of the kernel.
  It reaches the kernel through system calls (`fork`, `exec`, `read`, `write`).
  MiniOS's shell lives in the kernel only because MiniOS has no user space yet
  (ch.0). Splitting the shell into a user process is a great advanced exercise —
  it forces you to build system calls and a user/kernel boundary.
- **The init process**: on Unix, the kernel's job ends by starting *one* user
  program (PID 1, `init`/`systemd`), which starts everything else. The kernel then
  becomes pure event-loop-and-services, exactly like MiniOS's second half.
- **Run-to-completion vs. blocking**: MiniOS handlers must finish fast (they run
  with interrupts disabled). Real kernels let tasks *block* — sleep waiting for I/O
  and yield the CPU — which requires the scheduler above.

### Exercises

1. Add a `uptime` command that prints `tick / 100` seconds. Which two files do you
   touch, and which existing state do you read?
2. Explain precisely why `while(1) hlt;` uses ~0% CPU but `while(1){}` uses 100%.
   What does the CPU do differently in each?
3. The shell runs inside the keyboard interrupt's call chain. What constraint does
   that put on how long `shell_execute` may take, and why? (Hint: interrupt gates.)
4. Sketch the change to `timer_callback` that would begin round-robin switching
   between two tasks. What state must you save and restore?
