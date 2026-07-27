# Scheduling reference

MiniOS runs several ring-3 programs (three today) by switching between them on
every timer tick. This page documents how that switch works, why it is safe, and
the two things that are easy to get wrong. Read from `kernel/scheduler.c`,
`kernel/scheduler.h`, `kernel/timer.c`, `kernel/isr.c`, and `kernel/usermode.c`.
For the rationale and the trade-offs, see
[decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md) (the
switch), [decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md)
(dynamic tasks and stacks), and
[decision 0012](../decisions/0012-per-process-paging.md) (the per-task address
space the switch now also loads).

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

## A task is a saved frame plus an address space

`task_t` (`kernel/scheduler.h`) is a saved `registers_t` plus the address space
it runs in, a state, and an id:

```c
typedef struct {
    registers_t regs;          // the saved/forged interrupt frame: IS the task
    address_space_t *aspace;   // this task's private page-table tree
    uint64_t cr3;              // physical PML4 base to load on switch
    task_state_t state;        // TASK_UNUSED / READY / RUNNING / BLOCKED
    wait_reason_t wait_reason; // what it is blocked on, only while BLOCKED
    uint32_t id;
} task_t;
```

The `aspace` handle and its cached `cr3` are new: each task now owns a private
page-table tree, so two tasks can use the same virtual address for different
physical memory. The `cr3` field caches `aspace->pml4_phys` so the hot switch
path in `schedule()` need not chase the pointer. See
[paging.md](paging.md) and
[decision 0012](../decisions/0012-per-process-paging.md).

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
t->regs.rip      = entry;               // first instruction (0x400000 region)
t->regs.user_rsp = USER_STACK_TOP;      // fixed stack top, same VA in every task
t->regs.cs       = GDT_SELECTOR_USER_CODE;   // 0x1B, ring-3 code, RPL 3
t->regs.ss       = GDT_SELECTOR_USER_DATA;   // 0x23, ring-3 data, RPL 3
t->regs.rflags   = USER_MODE_RFLAGS;         // 0x202, IF set
t->state         = TASK_READY;
```

This is exactly the trick `enter_user_mode` (`kernel/usermode.c`) uses to drop to
ring 3, generalised into a table entry. The first time `schedule()` picks this
task, it copies this frame onto the stack and `iretq` "returns" into a program
that never actually ran.

`user_rsp` is now the SAME fixed address (`USER_STACK_TOP`, `0x800000`) for every
task, not a per-task stack top handed out by an allocator. That works because
each task has a private address space in which that one virtual address maps to
its own physical frames. Before `task_create` forges the frame it builds that
address space (a private tree with the ring-3 image copied to fresh frames at
`0x400000` and a fresh stack at `USER_STACK_TOP`), and records its CR3. See
[paging.md](paging.md).

`rflags` bit 9 (the interrupt flag, IF) **must** be set. A task entered with IF
clear runs with interrupts masked, so the timer never fires while it runs, so it
is never preempted: it would own the machine forever and no other task would run.
`USER_MODE_RFLAGS` (0x202) has bit 1 (reserved, always 1) and bit 9 (IF) set.

## The switch, and the two traps

`schedule(registers_t *r)` (`kernel/scheduler.c`) is the whole scheduler:

```c
tasks[current]->regs = *r;                       // 1. save interrupted frame
if (tasks[current]->state == TASK_RUNNING) {     //    ...but do not undo a block
    tasks[current]->state = TASK_READY;
}

int picked = find_next_ready(current);           // 2. round-robin pick, READY only
if (picked < 0) {                                // 2a. everyone blocked: sleep
    idle_until_runnable();
    picked = find_next_ready(current);
}
uint32_t next = (uint32_t)picked;
tasks[next]->state = TASK_RUNNING;

if (next == current) return;           // only one ready: do not switch to self
current = next;
*r = tasks[next]->regs;                // 3. OVERWRITE THE FRAME IN PLACE
__asm__ ("mov %0, %%cr3" :: "r"(tasks[next]->cr3));  // 4. SWITCH ADDRESS SPACES
```

Indexing is through the pointer array (`tasks[i]->regs`), and the round-robin
walk in `find_next_ready` is bounded by `num_tasks` (the count actually created)
rather than the old fixed `MAX_TASKS`. The save, pick, and overwrite are
otherwise identical to the pre-heap version; the additions are step 4, the CR3
load, and the two blocking-related changes marked above.

**The save is conditional now.** It used to set `TASK_READY` unconditionally,
which became wrong once a task could block: `task_block` marks the current task
`TASK_BLOCKED` and *then* drives this same function to switch away, so an
unconditional write here would put it straight back into the rotation and undo
the block on the spot. See [blocking.md](blocking.md).

**The pick tests for `TASK_READY`,** not "not unused", which is what makes a
blocked task invisible: the cursor steps over it however many times it comes
round, until whatever it waits for marks it `READY` again. If nothing at all is
runnable, `find_next_ready` returns -1 and `schedule()` parks in
`idle_until_runnable`, which sits in `sti; hlt` until an interrupt makes someone
ready. It does not spin, and it does not fall back on a blocked task, which would
resume a program in the middle of a wait it has not finished.

**Step 4: load the incoming task's CR3.** With per-process paging, switching the
register frame is only half the switch: the next task's code and stack live in
ITS tree, at the same virtual addresses the outgoing task used, so its CR3 must
be loaded too. Writing CR3 also flushes the TLB (MiniOS uses no global pages),
dropping the outgoing task's stale user translations for free. The switch is safe
mid-interrupt because everything the CPU still needs on the way out (the frame
`r` on the kernel stack, the `tasks[]` array and this code, and the IDT/GDT/TSS/
stub the next tick reaches) lives in the kernel half, which is cloned identically
into every tree, so only the user half changes. The CR3 write MUST come after the
scheduler finishes reading its own state and before `iretq`. See
[paging.md](paging.md) and
[decision 0012](../decisions/0012-per-process-paging.md).

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
| `TASK_BLOCKED` | Waiting for an event, skipped by the rotation entirely. Set by `task_block`, cleared back to `TASK_READY` by `scheduler_wake`. |

A task therefore yields the CPU two ways: involuntarily, by being preempted on a
timer tick, or voluntarily, by calling `task_block` at a syscall boundary. Both
go through this same `schedule()`; only the thing that prompted the call differs.
A blocked task also carries a `wait_reason_t` saying what it waits for, so the
right waker can find it. The mechanism, including why a block rewinds the saved
`rip` onto the `int 0x50` rather than resuming mid-syscall, is in
[blocking.md](blocking.md) and
[decision 0017](../decisions/0017-blocking-and-sleep.md).

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

Each task needs its own stack, reachable at CPL 3. With per-process paging every
task's stack lives at ONE fixed virtual address, the top of PD[3]:

```c
#define USER_STACK_SIZE  0x40000                         // 256 KB per task
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)   // top 0x800000
```

`build_user_space` (`kernel/scheduler.c`) maps that VA range to FRESH frames in
the task's private tree, so every task uses the same address but its own physical
memory. This is the change that retires the old bump allocator and the shared
2MB stack region: stacks no longer compete for one region, so the eight-stack
ceiling and the no-guard-page corruption from
[decision 0011](../decisions/0011-dynamic-tasks-and-stacks.md) are gone. The
frames still cannot come from the kernel heap (`kmalloc` returns pages with no
PG_USER bit, which a ring-3 push would fault on); they come from `alloc_frame`
and are mapped user-accessible by `paging_map_page`. See
[paging.md](paging.md) and [memory-map.md](memory-map.md).

## What a run looks like

All three programs loop forever calling `SYS_WRITE` with a single-letter string
and a crude busy-wait delay between writes (none calls `SYS_EXIT`). Booted under
QEMU, the screen fills with interleaved letters (order varies with slice timing):

```
ABCBCACBACBABCABCABCABC...
```

Under `-d int`, timer vector `0x40` fires continuously and syscall vector `0x50`
fires from all three tasks: entries cycle between `IP=001b:0040002b` (A),
`IP=001b:00400083` (B), and `IP=001b:004000db` (C), each at `cpl=3`, but now each
under a DIFFERENT `CR3` (one page-table tree per task), with no `#GP` (0x0D) and
no `#PF` (0x0E). Three distinct RIPs all at the same virtual addresses, under
three distinct CR3 values, are the proof that the tasks share virtual addresses
but not physical memory: the isolation. All three now run their stack at the same
VA (`0x800000` top), so the stack no longer distinguishes them in the log; the
CR3 does. A `#PF` at a user RIP would mean that task's private user mapping is
missing.

Failure modes to recognise: if one letter repeats forever, the frame is not being
written over `*r` (trap 1). If output stops after a single switch, the EOI is
being sent after the switch instead of before (trap 2). A triple fault right after
the first switch means something the kernel needs is not in the cloned kernel
half (see [paging.md](paging.md)).

## Related

- The decision and its trade-offs:
  [decision 0008](../decisions/0008-round-robin-preemptive-scheduler.md).
- The interrupt frame the scheduler swaps and the EOI path it relies on:
  [idt.md](idt.md).
- The ring-3 drop `task_create` generalises:
  [user-mode.md](user-mode.md).
- The syscall gate the tasks print through:
  [syscalls.md](syscalls.md).
- The blocked state, the re-arm, and the wakers:
  [blocking.md](blocking.md) and
  [decision 0017](../decisions/0017-blocking-and-sleep.md).
- The per-task address space the switch loads:
  [paging.md](paging.md).
- The fixed user virtual layout each task's stack sits in:
  [memory-map.md](memory-map.md).
