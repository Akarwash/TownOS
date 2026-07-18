# Architecture

MiniOS is a small x86-64 hobby kernel that boots via Multiboot, climbs into
64-bit long mode, and runs an interrupt-driven interactive shell. It is a
learning kernel: single address space and no filesystem. It drops to ring 3
(CPL 3) to run small demonstration programs in their own user-accessible pages,
and those programs call back into the kernel through a single `int 0x50` syscall
gate (`SYS_WRITE`, `SYS_EXIT`) rather than faulting. A round-robin preemptive
scheduler now switches between two ring-3 tasks on every timer tick by
overwriting the interrupt frame in place, so the two run concurrently. There is
still no address-space isolation, so this is cooperative-in-spirit multitasking
in a single shared space, not process isolation. See
[reference/user-mode.md](reference/user-mode.md),
[reference/syscalls.md](reference/syscalls.md), and
[reference/scheduling.md](reference/scheduling.md).

This page is a map, not a tutorial. For the concepts behind each subsystem, see
[`../learnings/`](../learnings/README.md) and follow the cross-links.

## Directory layout

| Directory | Responsibility |
|-----------|----------------|
| `boot/` | Multiboot header and the hand-written 32 to 64 long-mode climb (assembly). |
| `kernel/` | Core kernel: GDT/TSS, IDT, interrupt dispatch, syscall dispatch, timer, physical memory allocator, ring-3 entry, the scheduler, and `kernel_main`. |
| `drivers/` | Hardware drivers: VGA text screen, PS/2 keyboard, port I/O helpers. |
| `libc/` | Minimal freestanding C library: `string` and `mem` routines. |
| `shell/` | The interactive command shell. |
| `user/` | The two ring-3 demonstration programs (`.user_text`, run at CPL 3, call the kernel via `int 0x50`; the scheduler switches between them). |
| `include/` | Shared definitions (`types.h`, the vector map, the syscall ABI numbers). |

## Source files

| File | Responsibility | State |
|------|----------------|-------|
| `boot/boot.asm` | Multiboot header, page tables, PAE/EFER/paging, bootstrap GDT, far jump to 64-bit, call `kernel_main`. | Implemented |
| `kernel/kernel.c` | `kernel_main`: the init sequence, then creates two tasks and starts the scheduler. | Implemented |
| `kernel/gdt.c`, `kernel/gdt.h` | Kernel GDT (null, kernel code/data, user code/data) and 64-bit TSS; selector constants. | Implemented |
| `kernel/usermode.c`, `kernel/usermode.h` | `enter_user_mode`: forge the `iretq` frame and drop to ring 3. | Implemented |
| `kernel/syscall.c`, `kernel/syscall.h` | Syscall dispatcher: `syscall_handler` switches on RAX (`SYS_WRITE`, `SYS_EXIT`). | Implemented |
| `kernel/scheduler.c`, `kernel/scheduler.h` | Round-robin scheduler: `task_create` forges a task, `schedule` swaps the interrupt frame, `scheduler_start` enters task 0. | Implemented |
| `user/user_program.c` | The two ring-3 demo programs in `.user_text` (`user_program_a`/`_b`); each loops calling the kernel via `int 0x50`. | Implemented |
| `kernel/gdt_flush.asm` | `lgdt`, reload data segments, reload CS via far return, `ltr`. | Implemented |
| `kernel/idt.c`, `kernel/idt.h` | IDT table, `idt_set_entry`, PIC remap, IDT zeroing, `lidt`. | Implemented |
| `kernel/isr.c`, `kernel/isr.h` | C-side interrupt dispatch: `isr_install`, `isr_handler`, `irq_handler`, handler registration. | Implemented |
| `kernel/isr_stubs.asm` | `isr0`-`isr31`, `irq0`-`irq15` entry points and the common save/restore stubs. | Implemented |
| `kernel/timer.c`, `kernel/timer.h` | PIT driver: program channel 0, count ticks on IRQ 0, call `schedule` each tick. | Implemented |
| `kernel/memory.c`, `kernel/memory.h` | Bitmap physical frame allocator. | Implemented |
| `drivers/screen.c`, `drivers/screen.h` | VGA text output: `print_char`/`print_string`/`print_int`, scrolling, cursor. | Implemented |
| `drivers/keyboard.c`, `drivers/keyboard.h` | PS/2 keyboard driver on IRQ 1, scancode to ASCII. | Implemented |
| `drivers/ports.c`, `drivers/ports.h` | `in`/`out` port I/O wrappers. | Implemented |
| `libc/string.c`, `libc/string.h` | `strlen`, `strcmp`, `strcpy`. | Implemented |
| `libc/mem.c`, `libc/mem.h` | `memcpy`, `memset`. | Implemented |
| `shell/shell.c`, `shell/shell.h` | Command loop: buffer keypresses, dispatch `help`/`clear`/`hello`/`tick`. | Implemented |
| `include/types.h` | Fixed-width integer types and `NULL`. | Implemented |
| `include/vectors.h` | Single source of truth for every interrupt vector, including `SYSCALL_VECTOR`. | Implemented |
| `include/syscalls.h` | Standalone syscall ABI numbers (`SYS_EXIT`, `SYS_WRITE`), no kernel code. | Implemented |
| `linker.ld` | Section layout: kernel at 1M, `.user_text` / `.user_rodata` at 4M in their own `PT_LOAD` segment. | Implemented |
| `Makefile` | Build rules, toolchain, flags. | Implemented |

## Subsystem map

The boot-to-shell chain:

```
boot (boot.asm) ............ long-mode climb, hands off to kernel_main
  -> GDT/TSS (gdt.c) ....... segment descriptors + TSS, ltr
  -> IDT (idt.c) ........... PIC remap, IDT zero, set_entry, lidt
  -> ISR stubs (isr_stubs)   isr0-31 / irq0-15 entry points, common save/restore
  -> drivers .............. screen, keyboard, timer, ports
  -> task_create x2 ....... forge two ring-3 task frames (scheduler.c)
  -> scheduler_start ...... enter task 0 via enter_user_mode (usermode.c)
  -> user_program_a/_b .... run at CPL 3, call the kernel via int 0x50
  -> timer tick ........... schedule() swaps the interrupt frame in place
  -> syscall_handler ...... SYS_WRITE prints (syscall.c)
```

The kernel links into `minios.elf`, is repackaged as a Multiboot-loadable
`minios.bin`, and boots under QEMU. See [building.md](building.md) for the build
and run steps, [reference/idt.md](reference/idt.md) for the interrupt path, and
[reference/user-mode.md](reference/user-mode.md) for the ring-3 drop.

## Control flow

From power-on to the idle loop:

1. GRUB/QEMU reads the Multiboot header in `boot/boot.asm` and hands control to
   `_start` in 32-bit protected mode.
2. `_start` performs the 32 to 64 long-mode climb (page tables, PAE, EFER.LME,
   enable paging), loads a bootstrap GDT, and far-jumps into 64-bit code. See
   [reference/boot-sequence.md](reference/boot-sequence.md).
3. The 64-bit entry sets `RSP = stack_top` and calls `kernel_main`.
4. `kernel_main` (`kernel/kernel.c`) runs the init sequence in order:
   `gdt_init()`, `isr_install()`, `timer_init(100)`, `keyboard_init()`, clears
   the screen and prints the banner, then `memory_detect_and_map()` (reads the
   Multiboot map, extends the identity map to real RAM, flushes the TLB) and
   `memory_init()` (sizes the frame pool from the detected RAM). The banner
   reports the detected RAM. See
   [reference/memory-map.md](reference/memory-map.md).
5. `kernel_main` calls `task_create` for each of `user_program_a` and
   `user_program_b` (forging a ring-3 `iretq` frame per task), then
   `scheduler_start()`, which enters task 0 via `enter_user_mode`. From here the
   timer tick is a preemption point: `schedule()` saves the interrupted task's
   register frame and copies the next task's frame over it in place, so `iretq`
   resumes a different task. The two programs loop, each calling `SYS_WRITE`
   through the `int 0x50` gate, and interleave "A" and "B" on screen forever
   (neither calls `SYS_EXIT`). `scheduler_start` does not return, so the `hlt`
   idle loop below it is unreachable in this build. See
   [reference/scheduling.md](reference/scheduling.md),
   [reference/user-mode.md](reference/user-mode.md), and
   [reference/syscalls.md](reference/syscalls.md).

`kernel_main` takes the Multiboot info pointer (`boot/boot.asm` forwards EBX in
RDI) and is not expected to return. Interrupts stay enabled across the drop (each
task's forged RFLAGS keeps IF set), which is what lets the timer preempt a running
task and drive the switch.

## Where to read more

- Boot climb: [reference/boot-sequence.md](reference/boot-sequence.md)
- GDT/TSS: [reference/gdt.md](reference/gdt.md)
- Interrupts and the IDT: [reference/idt.md](reference/idt.md)
- Ring 3 and syscalls: [reference/user-mode.md](reference/user-mode.md), [reference/syscalls.md](reference/syscalls.md)
- The scheduler: [reference/scheduling.md](reference/scheduling.md)
- Memory layout: [reference/memory-map.md](reference/memory-map.md)
- Concepts (the why): [`../learnings/`](../learnings/README.md)
