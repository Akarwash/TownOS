# 0. What Is an Operating System?

Before any code, get the mental model right. Almost everything else follows from it.

## The big idea

An operating system is the program that sits between **raw hardware** and **the
programs you actually care about**. It exists to solve one recurring problem:
hardware is primitive, dangerous, and singular, but we want to run software that
is high-level, safe, and plural.

Concretely, every OS — from Linux to the firmware in a washing machine — does
some subset of four jobs:

1. **Abstraction.** Turn "write a byte to I/O port 0x60 and wait for an interrupt"
   into "read a line from the keyboard." Turn "sectors on a spinning platter" into
   "files and folders." The OS hides ugly hardware behind clean interfaces.

2. **Resource management.** There is one CPU (or a few cores), a finite amount of
   RAM, and one keyboard. If many programs want them, someone has to decide who
   gets what and when. That someone is the OS.

3. **Isolation and protection.** One program crashing or misbehaving should not
   corrupt another program or the OS itself. The OS, with help from the hardware,
   builds walls between programs.

4. **A uniform interface.** Programs should not have to know whether the disk is
   made by Samsung or Seagate. The OS presents one consistent API (system calls)
   regardless of the hardware underneath.

MiniOS is small enough that it only really does #1 (abstraction — it drives the
screen, keyboard, and timer) and touches #2 (it manages the CPU via the idle
loop). It does **not** yet do isolation or system calls, because it runs
everything in a single privilege level with no user programs. That is fine — you
learn the mechanisms first, then add the policies.

## The kernel / user split

The word "operating system" is fuzzy. The precise word is **kernel**: the core
piece of code that runs with full hardware privileges and is always resident in
memory. Around the kernel sits **user space**: shells, editors, browsers — normal
programs that must ask the kernel for anything privileged.

The CPU enforces this split with **privilege rings** (chapter 2). Ring 0 is the
kernel: it can execute any instruction and touch any hardware. Ring 3 is user
space: it is fenced in. When a user program needs something privileged (open a
file, send a network packet), it makes a **system call** — a controlled doorway
into the kernel.

```
+-------------------------------------------------+
|  user space (ring 3)                            |
|    shell, editor, browser ...                   |
+------------------------ | ----------------------+
                          |  system calls (the only legal door)
+------------------------ v ----------------------+
|  kernel (ring 0)                                |
|    scheduling, memory, drivers, filesystems     |
+------------------------ | ----------------------+
                          |  privileged instructions, port I/O
+------------------------ v ----------------------+
|  hardware: CPU, RAM, disk, keyboard, timer      |
+-------------------------------------------------+
```

MiniOS is **all kernel**. There is no ring 3, no user programs, no system calls.
The shell you will build runs in ring 0 alongside the drivers. This keeps the
project tractable while still teaching the real mechanisms. Adding a user/kernel
boundary is a natural "chapter 8" you could do later.

## Why "operating systems as a whole" and not just "this codebase"

The specific magic numbers in MiniOS (`0xB8000`, `0x3D4`, `0x1BADB002`) are x86-
and PC-specific trivia. You will forget them, and that is fine. What transfers to
*every* OS you ever touch is the **shape of the ideas**:

- A machine boots in a limited mode and the software must climb up to a capable one.
- Memory must be *described* to the CPU before it can be safely used.
- Hardware gets the CPU's attention through **interrupts**, not by the CPU asking.
- Drivers are just code that knows the private language of one device.
- Memory is handed out by an **allocator**, and protected by **paging**.
- The kernel spends most of its life *asleep*, woken by events.

Keep asking, for each MiniOS detail: *what is the general principle here, and how
would Linux solve the same problem at scale?* Each chapter answers that
explicitly in its "Going further" section.

## The four questions to carry through these docs

1. **Who is in control right now** — the firmware, the bootloader, or your kernel?
2. **What mode is the CPU in** — 16-bit real mode or 32-bit protected mode?
3. **How does control transfer** — a `call`, a far `jmp`, an interrupt, an `iret`?
4. **What does the hardware require** vs. what is a **choice the OS author made**?

If you can answer these at any point in the boot process, you understand what is
happening. Chapter 1 starts at the very first instruction.
