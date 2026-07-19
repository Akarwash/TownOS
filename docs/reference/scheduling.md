# Scheduling reference

MiniOS runs several ring-3 programs (three today) by switching between them on
every timer tick. This page documents how that switch works, why it is safe, and
the two things that are easy to get wrong. Read from `kernel/scheduler.c`,
`kernel/scheduler.h`, `kernel/timer.c`, `kernel/isr.c`, and `kernel/usermode.c`.
For the rationale and the trade-offs, see
[decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md) (the
switch) and [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md)
(dynamic tasks and stacks).

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

Each `task_t` is heap-allocated. `task_create` calls `kmalloc(sizeof(task_t))`
and stores the pointer in `task_t *tasks[MAX_TASKS_LIMIT]`, a flat pointer array
in creation order (`MAX_TASKS_LIMIT` = 64, an arbitrary and generous cap on the
bookkeeping array, not a storage ceiling: the structs live on the heap). A
pointer array rather than a linked list keeps `schedule()`'s round-robin indexing
O(1) and mechanical. Before [decision 0010](../decisions/0010-kernel-heap-ported-from-p5.md)
added the heap, this was a fixed `.bss` array of four (`MAX_TASKS`), the ceiling
that has now been removed. See
[decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md).

A `task_t` is kernel-only bookkeeping (only the scheduler reads it), never
touched by ring-3 code, so it is safe on kernel heap pages. That is what lets the
struct go on the heap while the stack (below) cannot.

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
tasks[current]->regs = *r;             // 1. save interrupted frame
tasks[current]->state = TASK_READY;

uint32_t next = current;               // 2. round-robin pick
for (uint32_t i = 1; i <= num_tasks; i++) {
    uint32_t cand = (current + i) % num_tasks;
    if (tasks[cand]->state == TASK_READY) { next = cand; break; }
}
tasks[next]->state = TASK_RUNNING;

if (next == current) return;           // only one ready: do not switch to self
current = next;
*r = tasks[next]->regs;                // 3. OVERWRITE THE FRAME IN PLACE
```

Indexing is through the pointer array now (`tasks[i]->regs`), and the round-robin
walk is bounded by `num_tasks` (the count actually created) rather than the old
fixed `MAX_TASKS`. That is the only change from the pre-heap version; the save,
pick, and overwrite are identical.

**Trap 1: the frame must be overwritten in place, through `r`.** `iretq` and the
stub's register pops read from the *stack*, not from the `tasks` array. Copying
`tasks[next]->regs` into a local variable, or anywhere but through the pointer `r`
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
only reconsidered at `i == num_tasks`, i.e. when nothing else is runnable. If it
is the only ready task, `next == current` and the function returns without
touching the frame, so a lone task simply resumes.

## Task states

| State | Meaning |
|-------|---------|
| `TASK_UNUSED` | Value 0. A freshly `kmalloc`'d `task_t` is set straight to `TASK_READY` by `task_create`, so a live task is never seen in this state; it exists as the zero value. |
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

## The user stacks

Each task needs its own stack, and each stack must be reachable at CPL 3. The
kernel heap is the wrong tool: it hands out frame-pool pages with no PG_USER bit,
so a `kmalloc`'d stack would page-fault the instant a ring-3 task pushed to it.
Stacks come instead from a tiny bump allocator, `alloc_user_stack`
(`kernel/scheduler.c`), which carves fixed-size slices out of the one PG_USER
stack page (2MB at 6-8M, PD[3]):

```c
#define USER_STACK_REGION_START  0x600000   // PD[3] base
#define USER_STACK_REGION_END    0x800000   // top of PD[3]
#define USER_STACK_SIZE          0x40000    // 256 KB per stack -> 8 stacks
```

`task_create` no longer takes a stack top; it asks the allocator for the next
slice and returns -1 if the region is exhausted. Today's three tasks get the
first three slices, growing down from `0x640000`, `0x680000`, and `0x6c0000`.

Heap-allocating the `task_t` (above) removed the *struct* ceiling, but this stack
region is a separate, still-hard ceiling: all stacks share this one fixed 2MB
region, so there are only 8, and there is no guard page between slices, so a task
that overflows its 256KB stack scribbles into its neighbour's. The real fix is
per-process paging (a `TODO(per-process-paging)` marks it in the source), where
each process gets its own address space and stacks stop competing for one shared
region. See [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md) and
[memory-map.md](memory-map.md).

## What a run looks like

All three programs loop forever calling `SYS_WRITE` with a single-letter string
and a crude busy-wait delay between writes (none calls `SYS_EXIT`). Booted under
QEMU, the screen fills with interleaved letters (order varies with slice timing):

```
ABCBCACBACBABCABCABCABC...
```

Under `-d int`, timer vector `0x40` fires continuously and syscall vector `0x50`
fires from all three tasks: entries cycle between `IP=001b:0040002b` (A) on a
stack near `0x640000`, `IP=001b:00400083` (B) near `0x680000`, and
`IP=001b:004000db` (C) near `0x6c0000`, each at `cpl=3`, with no `#GP` (0x0D) and
no `#PF` (0x0E). Three distinct RIPs and three distinct stacks in the log are the
proof the scheduler runs more than the old hardcoded two and each task keeps its
own context on its own dynamically-allocated stack. A `#PF` on a stack push would
mean the stack allocator handed out an address outside the PG_USER region.

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
