# Architecture

MiniOS is a small x86-64 hobby kernel that boots via Multiboot, climbs into
64-bit long mode, and runs an interrupt-driven interactive shell. It is a
learning kernel with no filesystem. It drops to ring 3 (CPL 3) to run small
demonstration programs in their own user-accessible pages, and those programs
call back into the kernel through a single `int 0x50` syscall gate (`SYS_WRITE`,
`SYS_EXIT`) rather than faulting. A round-robin preemptive scheduler switches
between several ring-3 tasks (three today) on every timer tick by overwriting the
interrupt frame in place, so they run concurrently. Each task is a heap-allocated
`task_t` with its OWN page-table tree (per-process paging): the scheduler loads
that task's CR3 on every switch, so two tasks share virtual addresses but not
physical memory. That is real address-space isolation, not a single shared space.
See [reference/user-mode.md](reference/user-mode.md),
[reference/syscalls.md](reference/syscalls.md),
[reference/scheduling.md](reference/scheduling.md), and
[reference/paging.md](reference/paging.md).

This page is a map, not a tutorial. For the concepts behind each subsystem, see
[`../learnings/`](../learnings/README.md) and follow the cross-links.

## Directory layout

| Directory | Responsibility |
|-----------|----------------|
| `boot/` | Multiboot header and the hand-written 32 to 64 long-mode climb (assembly). |
| `kernel/` | Core kernel: GDT/TSS, IDT, interrupt dispatch, syscall dispatch, timer, physical frame allocator, the kernel heap, per-process paging, ring-3 entry, the scheduler, and `kernel_main`. |
| `drivers/` | Hardware drivers: VGA text screen, PS/2 keyboard, the polled ATA PIO disk driver, port I/O helpers. |
| `libc/` | Minimal freestanding C library: `string` and `mem` routines. |
| `shell/` | The interactive command shell. |
| `user/` | The three ring-3 demonstration programs (`.user_text`, run at CPL 3, call the kernel via `int 0x50`; the scheduler switches between them). |
| `include/` | Shared definitions (`types.h`, the vector map, the syscall ABI numbers). |

## Source files

| File | Responsibility | State |
|------|----------------|-------|
| `boot/boot.asm` | Multiboot header, page tables, PAE/EFER/paging, bootstrap GDT, far jump to 64-bit, call `kernel_main`. | Implemented |
| `kernel/kernel.c` | `kernel_main`: the init sequence, then creates three tasks and starts the scheduler. | Implemented |
| `kernel/gdt.c`, `kernel/gdt.h` | Kernel GDT (null, kernel code/data, user code/data) and 64-bit TSS; selector constants. | Implemented |
| `kernel/usermode.c`, `kernel/usermode.h` | `enter_user_mode`: forge the `iretq` frame and drop to ring 3. | Implemented |
| `kernel/syscall.c`, `kernel/syscall.h` | Syscall dispatcher: `syscall_handler` switches on RAX (`SYS_WRITE`, `SYS_EXIT`). | Implemented |
| `kernel/scheduler.c`, `kernel/scheduler.h` | Round-robin scheduler: `task_create` heap-allocates and forges a task and builds its private address space, `schedule` swaps the interrupt frame and loads the next task's CR3, `scheduler_start` enters task 0. | Implemented |
| `kernel/paging.c`, `kernel/paging.h` | Per-process paging: `paging_create_address_space` (private tree, kernel half cloned by value), `paging_map_page` (4KB user mappings), `paging_switch` (load CR3). | Implemented |
| `user/user_program.c` | The three ring-3 demo programs in `.user_text` (`user_program_a`/`_b`/`_c`); each loops calling the kernel via `int 0x50`. | Implemented |
| `kernel/gdt_flush.asm` | `lgdt`, reload data segments, reload CS via far return, `ltr`. | Implemented |
| `kernel/idt.c`, `kernel/idt.h` | IDT table, `idt_set_entry`, PIC remap, IDT zeroing, `lidt`. | Implemented |
| `kernel/isr.c`, `kernel/isr.h` | C-side interrupt dispatch: `isr_install`, `isr_handler`, `irq_handler`, handler registration. | Implemented |
| `kernel/isr_stubs.asm` | `isr0`-`isr31`, `irq0`-`irq15` entry points and the common save/restore stubs. | Implemented |
| `kernel/timer.c`, `kernel/timer.h` | PIT driver: program channel 0, count ticks on IRQ 0, call `schedule` each tick. | Implemented |
| `kernel/memory.c`, `kernel/memory.h` | Bitmap physical frame allocator, plus `alloc_frames_contiguous` for multi-page runs. | Implemented |
| `kernel/heap.c`, `kernel/heap.h` | Kernel heap (`kmalloc`/`kfree`): explicit free list with boundary tags and coalescing, ported from p5, on top of the frame allocator. | Implemented |
| `drivers/screen.c`, `drivers/screen.h` | VGA text output: `print_char`/`print_string`/`print_int`, scrolling, cursor. | Implemented |
| `drivers/keyboard.c`, `drivers/keyboard.h` | PS/2 keyboard driver on IRQ 1, scancode to ASCII. | Implemented |
| `drivers/disk.c`, `drivers/disk.h` | Polled ATA PIO disk driver: `disk_init`/`disk_read`/`disk_write` move 512-byte LBA28 blocks on the primary bus. | Implemented |
| `drivers/ports.c`, `drivers/ports.h` | `in`/`out` port I/O wrappers (byte and word, in and out). | Implemented |
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
  -> heap_init ............ build the kernel heap (heap.c)
  -> disk_init ............ probe the primary ATA bus, silence IRQ14 (disk.c)
  -> task_create x3 ....... kmalloc + forge a ring-3 task, build its private address space (scheduler.c, paging.c)
  -> scheduler_start ...... load task 0's CR3, enter task 0 via enter_user_mode (usermode.c)
  -> user_program_a/_b/_c . run at CPL 3 in their own trees, call the kernel via int 0x50
  -> timer tick ........... schedule() swaps the interrupt frame and loads the next task's CR3
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
5. `kernel_main` calls `heap_init()` and then `disk_init()` (which probes the
   primary ATA bus, prints whether a disk was detected, and sets nIEN so the
   polled driver's IRQ14 stays silent), then `task_create` for each of
   `user_program_a`, `_b`, and `_c` (each `kmalloc`s a `task_t`, builds a private
   page-table tree with its own copy of the ring-3 image and stack, and forges its
   ring-3 `iretq` frame), then `scheduler_start()`, which loads task 0's CR3 and
   enters task 0 via `enter_user_mode`. From here the timer tick is a preemption
   point: `schedule()` saves the interrupted task's register frame, copies the
   next task's frame over it in place, and loads the next task's CR3, so `iretq`
   resumes a different task in its own address space. The three programs loop, each
   calling `SYS_WRITE` through the `int 0x50` gate, and interleave "A", "B", and
   "C" on screen forever (none calls `SYS_EXIT`). `scheduler_start` does not
   return, so the `hlt` idle loop below it is unreachable in this build. See
   [reference/scheduling.md](reference/scheduling.md),
   [reference/paging.md](reference/paging.md),
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
- Per-process paging: [reference/paging.md](reference/paging.md)
- Memory layout: [reference/memory-map.md](reference/memory-map.md)
- The kernel heap: [reference/heap.md](reference/heap.md)
- The disk driver: [reference/disk.md](reference/disk.md)
- Concepts (the why): [`../learnings/`](../learnings/README.md)
