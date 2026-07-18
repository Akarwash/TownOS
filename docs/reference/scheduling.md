# Scheduling reference

MiniOS runs two ring-3 programs by switching between them on every timer tick.
This page documents how that switch works, why it is safe, and the two things
that are easy to get wrong. Read from `kernel/scheduler.c`, `kernel/scheduler.h`,
`kernel/timer.c`, `kernel/isr.c`, and `kernel/usermode.c`. For the rationale and
the trade-offs, see
[decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md).

## The pile is the program

Every interrupt enters through a stub in `kernel/isr_stubs.asm` that pushes all
15 general-purpose registers, on top of the `rip/cs/rflags/rsp/ss` the CPU
already pushed, forming a `registers_t` (`kernel/isr.h`) on the kernel stack. The
stub loads a *pointer* to that on-stack frame into RDI and calls the C handler.
On return it pops those same registers and runs `iretq`.

That frame is a complete snapshot of the interrupted program: its instruction
pointer, its stack, its flags, its registers. Nothing else defines where a
program is. So switching tasks is nothing more than choosing which frame the stub
pops:

```
timer fires
  -> stub pushes the running task's registers  (registers_t on the kernel stack)
  -> irq_handler sends EOI, calls timer_callback(regs)
       -> schedule(regs):
            save   *regs           into tasks[current]
            pick   next ready task
            copy   tasks[next]     over *regs      <-- the switch
  -> stub pops registers (now the NEXT task's)
  -> iretq  -> resumes the next task
```

The scheduler never touches the CPU's registers directly. It edits the frame on
the stack and lets the interrupt path it did not write do the save and the
restore for it.

## A task is a saved frame

`task_t` (`kernel/scheduler.h`) is a saved `registers_t` plus a state and an id:

```c
typedef struct {
    registers_t regs;    // the saved/forged interrupt frame: IS the task
    task_state_t state;  // TASK_UNUSED / TASK_READY / TASK_RUNNING
    uint32_t id;
} task_t;
```

Tasks live in a fixed `.bss` array, `task_t tasks[MAX_TASKS]` (`MAX_TASKS` = 4).
The array is static because there is no kernel heap: the frame allocator
(`kernel/memory.c`) hands out physical addresses above the 8M identity map that
fault on first touch, so there is nowhere to allocate task structures. This is a
stopgap, noted as a TODO in the header.

`.bss` is zero-initialised, so every slot starts `TASK_UNUSED` (== 0) with a
zeroed frame, and no explicit table-clearing is needed.

## Forging a never-run task

A task that has never run has no saved frame to restore, so `task_create` forges
one that looks as if the task were interrupted at its first instruction:

```c
memset(&t->regs, 0, sizeof(t->regs));   // all GPRs 0
t->regs.rip      = entry;               // first instruction
t->regs.user_rsp = stack_top;           // top of this task's stack
t->regs.cs       = GDT_SELECTOR_USER_CODE;   // 0x1B, ring-3 code, RPL 3
t->regs.ss       = GDT_SELECTOR_USER_DATA;   // 0x23, ring-3 data, RPL 3
t->regs.rflags   = USER_MODE_RFLAGS;         // 0x202, IF set
t->state         = TASK_READY;
```

This is exactly the trick `enter_user_mode` (`kernel/usermode.c`) uses to drop to
ring 3, generalised into a table entry. The first time `schedule()` picks this
task, it copies this frame onto the stack and `iretq` "returns" into a program
that never actually ran.

`rflags` bit 9 (the interrupt flag, IF) **must** be set. A task entered with IF
clear runs with interrupts masked, so the timer never fires while it runs, so it
is never preempted: it would own the machine forever and no other task would run.
`USER_MODE_RFLAGS` (0x202) has bit 1 (reserved, always 1) and bit 9 (IF) set.

## The switch, and the two traps

`schedule(registers_t *r)` (`kernel/scheduler.c`) is the whole scheduler:

```c
tasks[current].regs = *r;              // 1. save interrupted frame
tasks[current].state = TASK_READY;

uint32_t next = current;               // 2. round-robin pick
for (uint32_t i = 1; i <= MAX_TASKS; i++) {
    uint32_t cand = (current + i) % MAX_TASKS;
    if (tasks[cand].state == TASK_READY) { next = cand; break; }
}
tasks[next].state = TASK_RUNNING;

if (next == current) return;           // only one ready: do not switch to self
current = next;
*r = tasks[next].regs;                 // 3. OVERWRITE THE FRAME IN PLACE
```

**Trap 1: the frame must be overwritten in place, through `r`.** `iretq` and the
stub's register pops read from the *stack*, not from the `tasks` array. Copying
`tasks[next].regs` into a local variable, or anywhere but through the pointer `r`
(which points at the live stack frame), would leave the on-stack frame unchanged,
and `iretq` would return to the *same* program. The switch only happens because
`*r = ...` writes over the frame the stub will pop.

**Trap 2: the EOI must go to the PIC before the switch.** It does, in
`irq_handler` (`kernel/isr.c`), which acks the PIC *before* calling the timer
callback that calls `schedule()`. Once `schedule()` overwrites the frame and the
stub `iretq`s into the next task, this handler invocation never returns. An EOI
sent *after* the switch would never execute, the timer line would stay masked,
and no further ticks would arrive: the machine would freeze after exactly one
switch. Because the ack already happened, the timer keeps firing across the
switch. See [idt.md](idt.md) for the EOI path.

The round-robin loop starts at `current + 1`, so the task just marked `READY` is
only reconsidered at `i == MAX_TASKS`, i.e. when nothing else is runnable. If it
is the only ready task, `next == current` and the function returns without
touching the frame, so a lone task simply resumes.

## Task states

| State | Meaning |
|-------|---------|
| `TASK_UNUSED` | Slot never filled. `.bss` zero-init lands every slot here. |
| `TASK_READY` | Runnable, waiting for a slice. Set by `task_create` and by `schedule` when a task is preempted. |
| `TASK_RUNNING` | Currently on the CPU. Exactly one task at a time. |

There is no blocked or sleeping state: a task yields the CPU only by being
preempted, never voluntarily.

## Starting, and the startup race

`scheduler_start()` marks task 0 running and enters it by reusing
`enter_user_mode` rather than hand-rolling a second `iretq`. Task 0's forged GPRs
(all zero) do not matter on this first entry: a fresh program sets up its own
registers before reading any. Every later entry into task 0 restores its full
saved frame through `schedule()`.

The timer starts ticking the instant `isr_install` runs `sti`, long before
`scheduler_start`, and each of those early ticks calls `schedule()` in kernel
(CPL 0) context where there is no task to switch. Two guards close the race:

- A `scheduler_running` flag makes `schedule()` a no-op until `scheduler_start`
  arms it, so early ticks do not save a kernel frame over a forged task or copy a
  forged task onto the kernel stack.
- `scheduler_start` runs `cli` to cover the handful of instructions between
  arming the scheduler and the `iretq` into task 0. The forged `rflags` (0x202)
  re-enables IF the moment ring-3 code begins, so the timer resumes immediately.

No locking is otherwise needed: interrupt gates clear IF on entry, so the timer
handler cannot nest, and it is the only place the shared `tasks`/`current` state
is touched.

## The two stacks

Each task needs its own stack, but there is only one PG_USER stack page (2MB at
6-8M, PD[3]). It is split in half:

| Task | Stack top | Range (grows down) |
|------|-----------|--------------------|
| 0 (`user_program_a`, "A") | `TASK0_STACK_TOP` = `0x700000` | 6-7M |
| 1 (`user_program_b`, "B") | `TASK1_STACK_TOP` = `0x800000` | 7-8M |

This is crude: a real system allocates a stack per task from an allocator instead
of carving one hard-coded page in two. There is no guard page between the two
halves, so a task that overflows its 1MB stack scribbles into the other's. See
[memory-map.md](memory-map.md).

## What a run looks like

Both programs loop forever calling `SYS_WRITE` with a single-letter string and a
crude busy-wait delay between writes (neither calls `SYS_EXIT`). Booted under
QEMU, the screen fills with interleaved letters:

```
ABABABABABABABAB...
```

Under `-d int`, timer vector `0x40` fires continuously and syscall vector `0x50`
fires from both tasks: entries alternate between one task at `IP=001b:0040002b`
on the `0x700000` stack and the other at `IP=001b:00400083` on the `0x800000`
stack, each at `cpl=3`, with no `#GP` (0x0D) and no `#PF` (0x0E). Two distinct
RIPs and two distinct stacks in the log are the proof the switch is real and each
task keeps its own context.

Failure modes to recognise: if one letter repeats forever, the frame is not being
written over `*r` (trap 1). If output stops after a single switch, the EOI is
being sent after the switch instead of before (trap 2).

## Related

- The decision and its trade-offs:
  [decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md).
- The interrupt frame the scheduler swaps and the EOI path it relies on:
  [idt.md](idt.md).
- The ring-3 drop `task_create` generalises:
  [user-mode.md](user-mode.md).
- The syscall gate both tasks print through:
  [syscalls.md](syscalls.md).
- The user stack page that is split in two:
  [memory-map.md](memory-map.md).
