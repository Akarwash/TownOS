# 0008 - A round-robin preemptive scheduler

## Status

Accepted. Partly superseded — the scheduling policy stands, four things around it
have moved on.

- **`task_create(entry, stack_top)`** no longer exists. The fixed task table and
  the hard-coded stack tops went to [0011](0011-dynamic-tasks-and-stacks.md)
  (heap-allocated structs) and [0012](0012-per-process-paging.md) (one stack VA
  per private tree); the function itself was replaced by `task_create_from_file`
  and `task_register` in [0015](0015-elf-program-loading.md).
- **Tasks compiled into the kernel** were superseded by
  [0015](0015-elf-program-loading.md): programs are ELF files on the disk now.
- **A task always runnable** was superseded by
  [0017](0017-blocking-and-sleep.md), which added `TASK_BLOCKED`, the wait
  reasons, and the idle loop for when nothing is ready.
- **Tasks that never end** were superseded by
  [0018](0018-process-lifecycle-exit-and-wait.md), which added `TASK_ZOMBIE`,
  `SYS_WAIT`, and the two-phase teardown.

Round-robin over a rotating index, preemption on the timer tick, and the
save-and-overwrite of the interrupt frame are all unchanged. See
[reference/scheduling.md](../reference/scheduling.md) for the current state.

## Context

MiniOS can drop to ring 3 (see [decision 0006](0006-user-mode-with-separate-pages.md))
and a ring-3 program can call back into the kernel (see
[decision 0007](0007-syscalls-via-int-0x50.md)), but only ever one program ran.
`kernel_main` dropped into a single hard-coded task and that task owned the CPU
until it called `SYS_EXIT`. The timer fired 100 times a second and did nothing
but increment a counter. There was one thread of control.

The machinery for switching between programs was, however, already almost all
present. Every interrupt (the timer included) enters through a stub in
`kernel/isr_stubs.asm` that pushes all 15 general-purpose registers plus the
CPU-pushed `rip/cs/rflags/rsp/ss` onto the kernel stack, forming a `registers_t`
(`kernel/isr.h`), and passes a **pointer** to that on-stack frame to the C
handler. On return the stub pops those same registers and runs `iretq`. That
frame is a complete snapshot of the interrupted program: it *is* the program's
context. Nothing else defines where a program is.

The question was how to switch tasks with what already exists, without a kernel
heap (the frame allocator returns unmapped addresses above the 8M identity map,
so there is nowhere to allocate task structures or per-task kernel stacks) and
without per-process address spaces (there is one shared identity-mapped space).

## Decision

**Preempt on the timer by overwriting the interrupt frame in place.** The timer
IRQ handler already receives a `registers_t*` into the live kernel stack. The
switch is three moves (`schedule()` in `kernel/scheduler.c`):

1. copy the interrupted frame `*r` into the current task's saved slot,
2. pick the next runnable task round-robin,
3. copy that task's saved frame back *over* `*r`, in place.

When the stub then pops the registers and runs `iretq`, it restores what it
believes is the same program but is really the next task. The pile is the
program; redirect the pile and you redirect execution. No separate context-switch
routine, no second stack to juggle: the interrupt path that was already there
does the save and the restore, and the scheduler only swaps which frame sits on
the stack.

**A fixed task table in `.bss`, not dynamic allocation.** `task_t tasks[MAX_TASKS]`
(`MAX_TASKS` = 4) is a static array. Each `task_t` is a saved `registers_t` plus a
state (`TASK_UNUSED`/`TASK_READY`/`TASK_RUNNING`) and an id. There is no working
kernel heap to allocate from (the frame allocator hands out unmapped frames above
8M), so a fixed table is the only option today. Recorded as a TODO in
`kernel/scheduler.h`.

**Forge a never-run task the same way the ring-3 drop does.**
`task_create(entry, stack_top)` fills a saved frame to look as if the task were
interrupted at its very first instruction: `rip` = entry, `user_rsp` = stack top,
`cs` = `GDT_SELECTOR_USER_CODE` (0x1B), `ss` = `GDT_SELECTOR_USER_DATA` (0x23),
`rflags` = `USER_MODE_RFLAGS` (0x202), all GPRs zero. This is exactly the trick
`enter_user_mode` uses to synthesise an `iretq` frame, generalised into a table
entry the scheduler can restore later. `rflags` bit 9 (IF) **must** be set: a task
entered with interrupts masked would never see a timer tick, never be preempted,
and own the machine forever.

**Two stacks by splitting the one user stack page.** There is a single 2MB
PG_USER page at 6-8M (PD[3]). It is split in half: task 0's stack top is
`0x700000`, task 1's is `0x800000`, each 1MB, each growing down. This is a crude
stopgap, hard-coded because there is no allocator to hand out a stack per task.

**Reuse `enter_user_mode` to start.** `scheduler_start()` marks task 0 running
and calls `enter_user_mode(task0.rip, task0.rsp)` rather than hand-rolling a
second `iretq`. Task 0's forged GPRs (all zero) are irrelevant on first entry: a
fresh program sets up its own registers before reading any. Every *later* entry
into task 0 goes through `schedule()`, which restores its full saved frame.

## Consequences

- **The timer tick is now a preemption point.** Two ring-3 programs
  (`user_program_a`, `user_program_b`) loop forever printing "A" and "B"; under
  QEMU they interleave `ABAB...` and the machine never settles on one. Neither
  calls `SYS_EXIT` (it stays implemented but unused) so the switching stays
  visible.

- **EOI must precede the switch, and does.** `irq_handler` (`kernel/isr.c`) sends
  the End-Of-Interrupt to the PIC *before* it calls the timer callback that calls
  `schedule()`. This ordering is load-bearing: once `schedule()` overwrites the
  frame and the stub `iretq`s into the next task, this handler invocation never
  returns, so an EOI sent *after* the switch would never run and the timer line
  would stay masked forever, freezing the machine after exactly one switch.
  Because the ack already happened, the timer keeps firing across the switch.

- **A startup race, closed two ways.** The timer starts ticking the moment
  `isr_install` runs `sti`, long before `scheduler_start`. Those early ticks fire
  in kernel (CPL 0) context with no task to switch. A `scheduler_running` guard
  makes `schedule()` a no-op until task 0 is entered, and `scheduler_start` runs
  `cli` to cover the handful of instructions between arming the scheduler and the
  `iretq` into task 0 (the forged `rflags` = 0x202 re-enables IF on entry).

- **No locking is needed.** Interrupt gates clear IF on entry (see
  [decision 0005](0005-self-describing-vector-map.md) and
  [../reference/idt.md](../reference/idt.md)), so a handler cannot be interrupted
  mid-switch. The scheduler touches shared state (`tasks`, `current`) only inside
  the timer handler, which cannot nest.

- **This is not multitasking in any full sense.** There is no address-space
  isolation (one shared identity-mapped space; a bug in one task can scribble on
  the other's stack), no blocking or sleeping (a task yields only by being
  preempted), no task exit that returns anywhere, and a fixed table of at most
  four tasks. Real isolation needs per-process page tables and a `CR3` switch on
  context switch; that is the next large step and is out of scope here. See
  [../project-status.md](../project-status.md).

- **The `registers_t` push-order coupling still applies.** The scheduler leans
  entirely on the on-stack frame matching the struct; the same comment warning in
  `kernel/isr_stubs.asm` and `kernel/isr.h` that they must change together now
  also protects the scheduler.

## Related

- The mechanism in depth: [../reference/scheduling.md](../reference/scheduling.md).
- The interrupt frame it swaps and the EOI ordering it depends on:
  [../reference/idt.md](../reference/idt.md).
- The ring-3 drop it generalises: [decision 0006](0006-user-mode-with-separate-pages.md)
  and [../reference/user-mode.md](../reference/user-mode.md).
- The syscall path both tasks use to print:
  [decision 0007](0007-syscalls-via-int-0x50.md) and
  [../reference/syscalls.md](../reference/syscalls.md).
- The stack split and why the frame pool is still unusable:
  [../reference/memory-map.md](../reference/memory-map.md).
