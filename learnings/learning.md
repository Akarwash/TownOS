# MiniOS: Understanding Your Operating System From the Ground Up

A learning guide that explains not just what every piece does, but why it exists, who designed it, and what their reasoning was. Read this alongside the code.

---

## Part 1: The Machine Wakes Up

### What Actually Happens at Power-On

When you press the power button on a computer, electricity flows into the CPU. The CPU is a piece of silicon with billions of transistors, and it does exactly one thing: fetch an instruction from memory, execute it, fetch the next one, forever.

But where does it get the first instruction? The CPU is hardwired (literally, in the silicon design) to start executing at address 0xFFFFFFF0. This address doesn't point to RAM. The motherboard's chipset routes it to a ROM chip containing the BIOS (Basic Input/Output System). The BIOS is firmware written by the motherboard manufacturer. It performs hardware checks (called POST — Power-On Self-Test), initializes devices, and then looks for something bootable.

The BIOS checks bootable devices in a configured order (hard drive, USB, CD). For each device, it reads the first 512 bytes (one sector) into memory at address 0x7C00 and checks if the last two bytes are 0x55 and 0xAA (a "boot signature"). If they are, the BIOS jumps to 0x7C00 and starts executing that code.

This 512-byte region is the Master Boot Record (MBR). Real bootloaders like GRUB use a multi-stage approach: stage 1 fits in the MBR and loads a larger stage 2 from disk, which handles filesystem reading, menu display, and kernel loading.

### Why We Use Multiboot Instead of Writing a Bootloader

Writing a bootloader from scratch teaches you about legacy x86 quirks from the 1980s, but not much about operating systems. You'd spend weeks dealing with:

**Real Mode**: The CPU starts in 16-bit mode, pretending to be an Intel 8086 from 1978. You can only address 1MB of memory. Instructions work differently. You'd have to use BIOS interrupt calls for I/O (a 1980s API that only works in 16-bit mode).

**The A20 Line**: On the original IBM PC AT (1984), IBM needed backwards compatibility with programs that relied on address wrapping at 1MB. Their solution was to physically gate address line 20, preventing access to memory above 1MB. To access all memory, you have to enable the A20 line. The horrifying part: IBM wired this to a spare pin on the keyboard controller (Intel 8042 chip), because it happened to have unused I/O pins. So enabling full memory access requires sending commands to the keyboard controller. This is widely regarded as one of the worst hardware design decisions in computing history.

**Disk Reading in Real Mode**: Loading the kernel from disk requires using BIOS interrupt 0x13, which means setting up specific registers, handling errors, and dealing with CHS (Cylinder-Head-Sector) addressing from the floppy disk era.

The Multiboot specification (created by the GNU Project for GRUB) solves all of this. It defines a contract: your kernel provides a magic header, and the bootloader handles all the legacy complexity. QEMU has a built-in Multiboot loader, so we don't even need to install GRUB.

### The Multiboot Header (boot.asm)

The header is three 4-byte values placed at the beginning of the binary:

**Magic number (0x1BADB002)**: GRUB scans the first 8192 bytes of the kernel binary looking for this exact value. It's an arbitrary identifier chosen by the Multiboot spec authors. It looks vaguely like "1 BAD BOOT" which may or may not be intentional.

**Flags (0x00000003)**: A bitfield requesting features from the bootloader. Bit 0 asks for page-aligned section loading (useful for paging later). Bit 1 asks for a memory map (so the kernel knows how much RAM exists).

**Checksum**: Calculated so that magic + flags + checksum = 0. This is an integrity check. If any value got corrupted during loading, the sum won't be zero and GRUB will reject the kernel.

### The Stack (boot.asm)

After GRUB jumps to _start, the CPU has no stack. GRUB deliberately doesn't set one up because it doesn't know how much stack space the kernel needs or where it should go.

The x86 stack grows downward. When you push a value, the stack pointer (ESP) decreases. When you pop, it increases. We reserve 16384 bytes (16KB) in the .bss section and point ESP at the top (high address). The stack then has room to grow downward toward the bottom.

`mov esp, stack_top` is the very first instruction the OS executes. Without it, nothing works: no function calls (call pushes a return address), no local variables (they live on the stack), nothing.

### Protected Mode

By the time GRUB hands control to us, it has already switched the CPU from 16-bit real mode to 32-bit protected mode. This gives us:

- 32-bit registers (EAX, EBX, etc. instead of AX, BX)
- Access to 4GB of address space (instead of 1MB)
- Memory protection via segmentation and paging
- Privilege levels (ring 0 for kernel, ring 3 for user programs)

Protected mode requires a GDT (Global Descriptor Table) to be set up. GRUB creates a temporary one, but the Multiboot spec says it's only valid during the handoff. We create our own immediately.

---

## Part 2: Memory — Two Ways to Talk to Hardware

### The Bus

The CPU is connected to other components through a bus — a set of electrical wires that carry data. RAM is on this bus, but so are other devices: the VGA controller, the keyboard controller, the timer chip, the disk controller. They all sit on the same highway, each responding to specific addresses.

When the CPU executes a memory write instruction, it puts an address and a data value on the bus. Every device checks: "is that my address?" Whoever claims it accepts the data.

### Memory-Mapped I/O (MMIO)

Some devices are mapped into the regular memory address space. The CPU doesn't know (or care) that it's talking to a device instead of RAM. It just reads and writes addresses.

The x86 address space below 1MB has a specific layout dictated by IBM's original PC design from 1981:

```
0x00000 - 0x9FFFF  Conventional RAM (640 KB)
0xA0000 - 0xBFFFF  Video Memory
   0xB8000-0xB8F9F    VGA text mode buffer (our screen)
0xC0000 - 0xFFFFF  BIOS ROM, device ROMs
0x100000 - onward  Extended RAM (free for kernel use)
```

The VGA text buffer at 0xB8000 is the most important example. Writing bytes there makes characters appear on screen. The CPU thinks it's writing to memory. The bus routes it to the VGA hardware instead.

### Why 640 KB?

Bill Gates allegedly said "640K ought to be enough for anybody" (he denies this). The real reason is simpler: when IBM designed the original PC in 1981, they allocated the first 640KB of the 1MB address space for RAM and reserved the rest for ROM and devices. Every x86 system since has maintained this layout for backwards compatibility, even though modern systems have gigabytes of RAM. The extra RAM is mapped above 1MB in the "extended memory" region.

### Port-Mapped I/O

x86 has a second, completely separate address space for device communication: I/O ports (0 to 65535). These are accessed with special CPU instructions (`in` and `out`) rather than regular memory reads/writes.

Port I/O exists for historical reasons. Intel's 8086 processor (1978) had separate memory and I/O buses because their earlier 8-bit processors (8080, 8085) had them, and they wanted backwards compatibility with peripheral chips designed for those older processors. The 8259 PIC, 8253 PIT, 8042 keyboard controller — these were all designed for the 8080/8085 I/O bus and carried forward to x86.

Memory-mapped I/O is better for large data transfers (like a screen buffer). Port I/O is better for small control commands (like "where is the cursor?" or "what key was pressed?").

### Why C Can't Do Port I/O

C was designed as a portable systems programming language. Port I/O is x86-specific — other architectures like ARM don't have it (they use memory-mapped I/O for everything). Since C doesn't have syntax for architecture-specific operations, we use inline assembly:

```c
__asm__("in %%dx, %%al" : "=a" (result) : "d" (port));
```

This tells the compiler: put `port` into the DX register (the input constraint "d"), execute the `in` instruction (which reads from the port in DX into AL), then copy AL into the C variable `result` (the output constraint "=a").

The double %% is because GCC's inline assembly uses % for its own placeholder syntax, so %% means a literal register name.

---

## Part 3: The Screen — VGA Text Mode

### How VGA Works

VGA (Video Graphics Array) was introduced by IBM in 1987 with the PS/2 computer line. It replaced earlier standards (CGA, EGA) and became the universal baseline for PC displays. Every x86 system, including modern ones, starts in VGA-compatible mode.

In text mode, the screen is an 80-column by 25-row grid. The VGA controller maintains a buffer of 4000 bytes (80 × 25 × 2) starting at address 0xB8000. Each character cell uses 2 bytes:

**Byte 0: ASCII character code** — The character to display (e.g., 65 = 'A', 72 = 'H')

**Byte 1: Color attribute** — Foreground and background colors packed into one byte:
- Bits 0-3: foreground color (16 options)
- Bits 4-6: background color (8 options)
- Bit 7: blink (we ignore this)

The value 0x0F means: foreground = 0xF (white), background = 0x0 (black).

The VGA controller has its own clock and processor. About 60 times per second, it reads through its entire buffer and draws the corresponding characters on the monitor. This happens independently of the CPU. You write to the buffer; the VGA controller reads it. That's the entire relationship.

### The Cursor

The cursor (the blinking line or block) is NOT a character in the video buffer. It's a visual overlay drawn by the VGA controller on top of whatever character is at the cursor position. The position is stored in two internal VGA registers (14 and 15), accessed through the index/data port pattern on ports 0x3D4 and 0x3D5.

The VGA controller (based on the Motorola 6845 CRTC — Cathode Ray Tube Controller, designed in 1977) has about 25 internal registers controlling various aspects of display timing, cursor shape, screen scrolling, and more. Since it only has two port addresses, it uses an index/data protocol: write the register number to port 0x3D4, then read or write the value through port 0x3D5.

The cursor position is a 16-bit number (0 to 1999) split across two 8-bit registers because each register can only hold one byte (0 to 255). Register 14 holds the high byte, register 15 holds the low byte. We combine them: (high << 8) | low.

### Scrolling

When the cursor moves past row 24 (the last row), the screen needs to scroll. We do this in software: copy rows 1-24 to positions 0-23 (using memcpy), then fill row 24 with spaces. The cursor moves to the beginning of the last row.

Hardware scrolling is also possible — the VGA has registers 12 and 13 (Start Address) that control where in the buffer the display begins reading. Changing this shifts the entire display instantly without copying memory. We use software scrolling because it's simpler to understand.

---

## Part 4: The GDT — Configuring Memory Segments

### Why Segmentation Exists

Intel introduced segmentation in the 8086 (1978) as a way to access more than 64KB of memory with 16-bit registers. A segment register provides a base address, and you access memory relative to that base. This was a pragmatic solution for 1978 hardware.

When Intel designed the 80286 (1982) and 80386 (1985), they kept segmentation but added protection: each segment descriptor specifies permissions (read, write, execute) and a privilege level. The OS sets up segments for its own code and data, and for each user process. If a user process tries to write to a kernel segment, the CPU generates a fault.

### Why We Use a Flat Model

Modern operating systems (Linux, Windows, macOS) don't use segmentation for memory protection. They use paging instead, which is more flexible. But x86 requires the GDT to exist — you literally cannot disable it in protected mode. So the standard approach is a "flat model": every segment covers the entire 4GB address space (base = 0, limit = 4GB). This makes segmentation invisible, as if it doesn't exist. Paging handles the real memory protection.

### The Descriptor Format

Intel designed the GDT entry format for backwards compatibility with the 286, resulting in a layout where the base address and limit are scattered across non-contiguous fields in the 8-byte entry. This is universally regarded as ugly, but it's what the hardware expects. Your struct must match it byte-for-byte (hence `__attribute__((packed))` to prevent the compiler from adding padding).

### The Three Entries

**Null descriptor (index 0)**: The CPU requires the first GDT entry to be all zeros. Any attempt to load a segment selector pointing to entry 0 causes a General Protection Fault. This is a safety feature: uninitialized segment registers (which default to 0) will fault instead of silently pointing to valid memory.

**Kernel code segment (index 1, offset 0x08)**: Base = 0, limit = 4GB, executable, readable, privilege level 0 (kernel). The CPU uses this segment when fetching instructions. The offset is 0x08 because entry 1 × 8 bytes per entry = 8.

**Kernel data segment (index 2, offset 0x10)**: Base = 0, limit = 4GB, writable, readable, privilege level 0. Used for all data access (variables, stack, memory-mapped I/O). Offset = 2 × 8 = 16 = 0x10.

### Loading the GDT

The `lgdt` instruction reads a 6-byte structure (2-byte limit + 4-byte base address) and stores it in the CPU's internal GDTR (GDT Register). After this, the CPU knows where to find the GDT.

But loading a new GDT doesn't automatically update the cached segment information. You must reload every segment register. The data segments (DS, ES, FS, GS, SS) can be set with `mov`. The code segment (CS) can only be changed via a far jump — an instruction that simultaneously changes both CS and EIP. `jmp 0x08:.flush` jumps to the very next instruction, but the side effect is CS gets loaded with 0x08 (our kernel code segment).

---

## Part 5: Interrupts — Making the OS React

### The Problem

Without interrupts, an OS can't react to anything. The keyboard could be pressed, but the CPU has no way to know unless it constantly checks (called "polling"). Polling wastes CPU time and is unreliable — you might miss events between checks.

Interrupts solve this. They're signals that say "stop what you're doing, something needs attention." The CPU finishes its current instruction, saves its state, jumps to a handler function, handles the event, restores its state, and continues where it left off.

### Who Designed This?

The concept of hardware interrupts dates to the 1950s (UNIVAC 1103A, 1954). The specific x86 interrupt mechanism was designed for the Intel 8086 (1978) and refined for the 80386 (1985). The IDT (Interrupt Descriptor Table) format in protected mode was introduced with the 286/386.

### Three Types

**Exceptions (0-31)**: Generated by the CPU itself when something goes wrong. Division by zero (exception 0), invalid opcode (6), page fault (14), general protection fault (13). These are synchronous — they happen as a direct result of the instruction being executed. Intel reserved numbers 0-31 for these; you don't get to choose.

**Hardware Interrupts (IRQs)**: Generated by external devices. The keyboard controller sends IRQ 1 when a key is pressed. The timer sends IRQ 0 on every tick. These are asynchronous — they can happen at any point during execution. The PIC maps them to interrupt numbers 32-47 (after remapping).

**Software Interrupts**: Triggered intentionally by the `int N` instruction. Linux uses `int 0x80` for system calls (user programs asking the kernel to do something). We won't use these yet.

### The IDT (Interrupt Descriptor Table)

The IDT is an array of 256 entries, one per interrupt number. Each entry tells the CPU: "for this interrupt, jump to this handler function at this address, using this code segment."

The entry format is similar to a GDT entry — 8 bytes with the handler address split across two fields (base_low and base_high) for backwards compatibility.

The flags byte encodes:
- Present bit (entry is valid)
- Privilege level (who can trigger this via software INT instruction)
- Gate type (0xE = 32-bit interrupt gate, which auto-disables interrupts during handling)

We load the IDT using the `lidt` instruction, just like `lgdt` for the GDT.

### The PIC (Programmable Interrupt Controller)

The Intel 8259A PIC (1976) sits between hardware devices and the CPU. When a device asserts its IRQ line, the PIC interrupts the CPU and tells it which interrupt number to invoke.

**Why two PICs?** The 8259A only handles 8 IRQ lines. IBM's original PC (1981) had one PIC handling IRQs 0-7. When they designed the PC AT (1984), they needed more IRQ lines, so they chained a second PIC (the "slave") to IRQ 2 of the first PIC (the "master"). This gives 15 usable IRQs (8 + 8 - 1, since IRQ 2 is used for the cascade).

**The remapping problem:** By default, the BIOS maps IRQs 0-7 to interrupts 0-7. But Intel reserved interrupts 0-31 for CPU exceptions. IRQ 0 (timer) collides with interrupt 0 (divide error). If the timer fires, the CPU can't tell if it's a timer tick or a divide-by-zero. We remap the master PIC to interrupts 32-39 and the slave to 40-47 by sending a specific initialization sequence (ICW1-ICW4) to the PIC's ports.

The ICW (Initialization Command Word) protocol is defined by the 8259A datasheet. It's a fixed 4-step sequence: ICW1 starts initialization, ICW2 sets the vector offset, ICW3 configures the cascade wiring, ICW4 sets the operating mode. The values are specific to the chip's design.

### ISR Stubs (isr_stubs.asm)

When an interrupt fires, the CPU automatically pushes some state onto the stack: EIP (where to return), CS, EFLAGS (processor flags). Some exceptions also push an error code. But the CPU does NOT save the general-purpose registers (EAX, EBX, etc.). If the handler modifies them, the interrupted code's register values are destroyed.

The ISR stubs bridge this gap. Each stub is a tiny assembly routine that:
1. Pushes a dummy error code (0) if the exception doesn't push one (to keep the stack consistent)
2. Pushes the interrupt number
3. Jumps to a common handler that saves ALL registers (pusha), switches to the kernel data segment, calls the C handler, restores all registers (popa), cleans up the stack, and returns from the interrupt (iret)

We need 32 stubs for CPU exceptions (isr0-isr31) and 16 for hardware IRQs (irq0-irq15). Only exceptions 8, 10, 11, 12, 13, 14, 17, and 21 push error codes; all others need the dummy push. Getting this wrong by even one entry shifts the entire stack layout and every field in registers_t reads garbage.

### The registers_t Struct

This struct maps to the exact stack layout at the moment the C handler is called:

```
(top of stack, lowest address)
  ds           ← we pushed this (original data segment)
  edi,esi,ebp,esp,ebx,edx,ecx,eax  ← pusha pushed these
  int_no       ← our stub pushed this
  err_code     ← either the CPU or our stub pushed this
  eip          ← the CPU pushed these automatically
  cs
  eflags
  user_esp     ← only if privilege level changed
  user_ss
(bottom of stack, highest address)
```

The struct is defined in the same order so that a pointer to the stack IS a pointer to a registers_t. The C handler receives this pointer and can inspect every register value from the moment the interrupt fired.

### EOI (End of Interrupt)

After handling a hardware interrupt, you must send the byte 0x20 to the PIC's command port (0x20 for master, 0xA0 for slave). This is called EOI — End of Interrupt. It tells the PIC "I've handled this, you can send me the next one."

If you forget EOI, the PIC thinks you're still busy and will never send another interrupt from that IRQ line. Your keyboard will stop responding, your timer will stop ticking, and the OS appears frozen.

For interrupts from the slave PIC (IRQ 8-15, interrupts 40-47), you must send EOI to both the slave AND the master, since the master's IRQ 2 (the cascade line) also needs to be acknowledged.

---

## Part 6: The Keyboard — User Input

### How Keyboard Input Works

The PS/2 keyboard controller (originally the Intel 8042 chip, 1981) sits at I/O ports 0x60 (data) and 0x64 (status/command). When you press a key, the keyboard controller:

1. Sends IRQ 1 to the PIC
2. The PIC sends interrupt 33 to the CPU (after remapping)
3. Our ISR stub runs, saves state, calls irq_handler
4. irq_handler calls our keyboard_callback
5. keyboard_callback reads the scan code from port 0x60

### Scan Codes vs ASCII

The keyboard doesn't send ASCII characters. It sends **scan codes** — numbers identifying which physical key was pressed or released. Scan code 0x1E is the 'A' key regardless of whether it's uppercase, lowercase, or combined with Ctrl. The OS is responsible for translating scan codes to characters based on the keyboard layout.

Key press events have scan codes 0x01-0x7F. Key release events have the same code with bit 7 set (0x81-0xFF). We only care about presses; we ignore releases (where scancode & 0x80 is true).

We use a lookup table that maps scan codes to ASCII characters. Only printable keys and special keys (Enter, Backspace, Space) have entries; the rest are 0 (ignored).

### Why QEMU Uses PS/2

Real modern keyboards use USB, which is incredibly complex to program at the hardware level (the USB specification is thousands of pages). QEMU emulates a PS/2 keyboard because it's simple: one port for data, one IRQ, scan codes in, done. This is also why your OS development experience is realistic — real hobby OS developers use PS/2 for the same reason.

---

## Part 7: The Timer — Keeping Time

### The PIT (Programmable Interval Timer)

The Intel 8253 PIT (1981) is a chip that generates periodic interrupts. It has an internal counter that decrements at a fixed rate of 1,193,182 Hz (this bizarre frequency was chosen because it's 1/3 of the NTSC color burst frequency, 3.579545 MHz, which was a convenient clock already available on the original IBM PC's motherboard).

You program it by setting a divisor. The PIT divides its base frequency by your divisor to determine how often it fires. A divisor of 11932 gives roughly 100 Hz (100 ticks per second).

The PIT fires IRQ 0, which after remapping becomes interrupt 32. Our timer handler just increments a counter. Later, this counter would be used for scheduling (switching between processes), implementing sleep(), and keeping wall-clock time.

---

## Part 8: The Shell — Making It Feel Like an OS

### What a Shell Is

A shell is the user-facing interface to the OS. It displays a prompt, waits for input, interprets commands, and shows results. Every OS has one in some form: bash on Linux, cmd on Windows, zsh on macOS.

Our shell is minimal. It maintains a character buffer. The keyboard handler feeds characters into the buffer. When Enter is pressed, the shell compares the buffer against known commands using strcmp and executes the matching action.

This is where all the pieces come together:
- Keyboard interrupt fires → ISR stub saves state → irq_handler calls keyboard_callback → callback reads scan code from port 0x60 → looks up ASCII → calls shell_handle_keypress
- shell_handle_keypress adds the character to the buffer and echoes it to screen via print_char (which writes to the VGA buffer at 0xB8000 and updates the cursor via port I/O to 0x3D4/0x3D5)
- On Enter, shell_execute uses strcmp to match the command and calls the appropriate function

### The Idle Loop

After initialization, kernel_main enters an infinite loop:

```c
while(1) { __asm__ __volatile__("hlt"); }
```

`hlt` puts the CPU into a low-power state until an interrupt arrives. When the keyboard fires an interrupt, the CPU wakes up, handles it, and goes back to sleep. This is better than a busy loop (`while(1) {}`) which wastes power spinning the CPU at full speed doing nothing.

---

## Part 9: The Big Picture

When MiniOS boots, here's the complete chain of events:

1. QEMU simulates power-on
2. BIOS runs, finds our kernel
3. Multiboot loader reads the header, loads kernel at 1MB
4. CPU switches to 32-bit protected mode
5. Jumps to _start in boot.asm
6. Stack is set up (mov esp, stack_top)
7. kernel_main is called
8. GDT is created and loaded (flat model, all memory accessible)
9. IDT is created, PIC is remapped, ISR stubs are registered
10. Timer is configured to 100 Hz on interrupt 32
11. Keyboard handler is registered on interrupt 33
12. Interrupts are enabled (sti)
13. Screen is cleared, welcome message is printed
14. Shell prompt is displayed
15. CPU enters idle loop (hlt until interrupt)
16. User presses a key → IRQ 1 → PIC → interrupt 33 → ISR stub → C handler → scan code read → ASCII lookup → character added to buffer → echoed to screen
17. User presses Enter → command matched → action executed → new prompt
18. Repeat forever

Every piece we built serves a specific role in this chain. Remove any one piece and the chain breaks.