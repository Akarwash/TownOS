# 0018 - Process lifecycle: exit, wait, and two-phase death

## Status

Accepted.

## Context

Tasks could be born but not die. `task_create_from_file` built a private address
space, copied a program into it, and added a `task_t` to the rotation
([0011](0011-dynamic-tasks-and-stacks.md),
[0012](0012-per-process-paging.md)), and nothing ever took one out again.
`num_tasks` only went up. Ids were handed out `0, 1, 2, ...` and never reused. The
comment in `paging_create_address_space` shrugged off a leak on a failed create
with "tasks are never destroyed once built", and it was right.

The consequences compounded. Every program ever run since boot was still standing
in the round-robin, being handed a slice forty times a second to do nothing with,
holding its 256KB stack and its copy of its own text. `SYS_EXIT` could only halt
the whole machine with `cli; hlt`, which was the honest thing to do when there was
no parent to return to but made "the program finished" and "the computer stopped"
the same event. The three demo programs looped forever because there was nothing
better for them to do. And the shell's `run` was a detach: it printed
`run: started A.ELF` and immediately went back to its prompt, with no way to know
whether the program had finished, let alone whether it had succeeded.

Blocking ([0017](0017-blocking-and-sleep.md)) supplied the missing primitive. A
task can now be taken out of the rotation and put back by whatever causes the
thing it waits for. "Wait for a child to finish" is exactly that shape, and
`WAIT_CHILD` was named in that decision as the next reason to slot in.

## Decision

Add `SYS_EXIT`-with-a-status and `SYS_WAIT`, a `TASK_ZOMBIE` state, and a teardown
path for an address space, with cleanup split between a sweeper in the scheduler
and the waiting parent.

**Two-phase death.** `task_exit` does paperwork only: mask the status, store it,
mark the task `TASK_ZOMBIE`, wake the parent, and switch away. It frees **nothing**.
It cannot: the task calling `SYS_EXIT` is the task currently on the CPU, its stack
is the stack, and its address space is the tree CR3 points at. Freeing any of that
from inside the call would return the running machine's own memory to the
allocator. Nothing faults at that moment, which is what makes it dangerous; the
frames are handed out to somebody else later and the machine dies somewhere
unrelated with no message that points anywhere near here.

**Split cleanup ownership.** The heavy resource (the address space: page tables,
the code copy, the 256KB stack) is freed by a sweeper, `reap_sweep()`, called at
the top of `schedule()`. It walks the table, finds zombies, and tears down their
trees — **except `current`**, which is the entire safety argument in one line. By
the time `schedule()` runs again, someone else is on the CPU with their own CR3
loaded, so the dead task's tree is nobody's ground to stand on.

The light resource (the `task_t` struct and its slot) is freed by the parent, at
`SYS_WAIT`, because that struct *is* the answer the parent came for. It holds the
exit status, and the parent is the only one who can say the status has been
collected and the tombstone is no longer needed.

**Orphans lose their tombstone; there is no reparenting.** A zombie whose parent is
already gone has nobody who can ever collect its status, so the sweeper frees the
struct and NULLs the slot on the spot rather than leaving a record no one will ever
read. Unix hands orphans to `init` instead; MiniOS has no `init`, and inventing one
solely to reap orphans is a lot of machinery for a case that resolves itself.

**`wait` is any-child, with no arguments.** `SYS_WAIT` blocks until *any* child of
the caller exits and returns that child's status. It is not `waitpid`: a caller with
several children is told about whichever it finds finished first and cannot ask
about a particular one. That is enough for the shell, which has exactly one child at
a time, and it keeps the syscall argument-free.

If the caller has no children at all, `SYS_WAIT` returns `SYSCALL_ERROR` rather than
blocking. Blocking would be a permanent, silent hang on an event that cannot happen,
which is the worst possible answer to a programming mistake.

**No id reuse.** A freed slot is set to NULL and left empty forever; `num_tasks`
becomes a high water mark rather than a live count, and every walk of `tasks[]`
skips NULL entries. Reusing ids would make a stale `parent_id` — recorded by a child
whose parent has since exited — name a *different*, live task. A `parent_id` that
can only ever name nothing is a much easier thing to reason about than one that
might name a stranger.

**No crt0: programs call `sys_exit` themselves.** Nothing wraps `_start`, so there
is no place to put an automatic exit without inventing a startup object file and a
loader that can resolve it. Each program ends itself at the bottom of `_start`.

**The exit status is masked to 0..255.** `SYS_WAIT` returns the status in RAX and
returns `SYSCALL_ERROR` (`(uint64_t)-1`) for "no children". An unmasked status of -1
would be indistinguishable from that error. Masking makes the two ranges disjoint by
construction rather than by everyone remembering not to exit -1.

## Consequences

- **`run` becomes a command instead of a detach.** `run A.ELF` prints A's output and
  then `run: A.ELF exited with status 0`, and the prompt reappears only after A is
  finished. `run C.ELF` reports status 3, which is the proof that the number
  survives the whole trip: from the child's RDI, through the mask, into the zombie's
  `exit_status`, out through the parent's RAX, and into a `print_uint` in ring 3.

- **Memory actually comes back.** Ten consecutive `run A.ELF` return the free-frame
  count to the same value every time. A steady count across repeated runs is the
  only thing that distinguishes "we free most of it" from "we free all of it".

- **`SYS_EXIT` no longer halts the machine.** Its old meaning is gone. A program that
  calls it leaves the rotation; the machine carries on.

- **A program that never exits hangs the shell.** There is no way to kill a task and
  there are no signals, so a child with an unbounded loop leaves the parent blocked
  in `SYS_WAIT` with no way back short of a reboot. This is why `user/A.c`, `B.c`,
  and `C.c` now run a fixed number of rounds. `TODO(kill-and-signals)`.

- **A program that falls off the end of `_start` runs into whatever follows it.**
  With no crt0 there is no safety net: forgetting `sys_exit` is undefined behaviour
  rather than an implicit `exit(0)`.

- **`wait` cannot target a particular child.** With more than one child the caller
  gets whichever is found finished first and cannot ask about a specific one, so
  there is no way to say "wait for *that* one". A `waitpid`-style call taking an id
  is the natural extension and needs no new mechanism, only an argument and a
  narrower scan.

- **Orphans are not reparented.** Their status is discarded rather than being
  collected by anyone. Nothing today can observe this, because the only task with no
  parent is the shell and nothing outlives it.

- **The table is sparse now, and every walk must know it.** `tasks[]` has NULL holes,
  `num_tasks` is a high water mark, and a walk without a NULL check faults inside
  the scheduler. `any_task_ready`, `find_next_ready`, and `scheduler_wake` each
  begin with one, and `schedule()` carries a defensive early return for a NULL
  `current` that should be unreachable.

- **`paging_destroy_address_space` has a precondition with no check behind it.** The
  tree passed to it must not be the one in CR3, and violating it does not fault
  there. Its second sharp edge is the same shape: it frees exactly two PD entries by
  index (`USER_PD_INDEX_CODE`, `USER_PD_INDEX_STACK`), and a generic "free everything
  present" loop would return the shared kernel mappings to the allocator and kill the
  machine minutes later, somewhere else entirely.

- **The RAX rule now covers two syscalls.** `SYS_WAIT` joins `SYS_READKEY` in writing
  `regs->rax` itself, only on the paths that have an answer, because the re-armed
  `int` reads the syscall number from RAX. `SYS_EXIT` writes it never. The dispatcher
  cases for all three are bare statements with no `regs->rax =` in front, and that is
  load-bearing rather than stylistic.

- **`WAIT_CHILD` proves the blocking mechanism generalises.** It needed a new enum
  value and a wake in the right place, and no change at all to `task_block`,
  `schedule()`, or the idle path. Its waker is another *task* rather than a driver,
  which the pairing rule already allowed for: whoever causes the event does the
  waking, and here that is the exiting child.

- **The `WAIT_CHILD` wake is by id, not by reason.** Unlike `WAIT_KEY`, which
  broadcasts, a child wakes its own parent specifically. Every parent in the system
  waits on the same reason, so a broadcast would ready all of them to re-issue
  `SYS_WAIT`, find none of their own children finished, and block again.

## Related

- The blocking primitive this builds on, and the `WAIT_CHILD` seam it fills:
  [0017](0017-blocking-and-sleep.md),
  [../reference/blocking.md](../reference/blocking.md).
- The scheduler and task table this changes:
  [0008](0008-round-robin-preemptive-scheduler.md),
  [0011](0011-dynamic-tasks-and-stacks.md),
  [../reference/scheduling.md](../reference/scheduling.md).
- The address space the teardown walks:
  [0012](0012-per-process-paging.md),
  [../reference/paging.md](../reference/paging.md).
- The syscall gate and the RAX rule: [0007](0007-syscalls-via-int-0x50.md),
  [../reference/syscalls.md](../reference/syscalls.md).
- The shell whose `run` now waits: [0016](0016-interactive-shell.md),
  [../reference/shell.md](../reference/shell.md).
- Concepts: [`../../learnings/20-process-lifecycle.md`](../../learnings/20-process-lifecycle.md).
