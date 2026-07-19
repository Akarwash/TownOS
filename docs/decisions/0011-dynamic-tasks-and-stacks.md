# 0011 - Heap-allocate task structs and bump-allocate user stacks

## Status

Accepted.

## Context

The scheduler carried two fixed limits, both of which existed only because there
was no kernel heap when it was written.

- The task table was a fixed `.bss` array, `task_t tasks[MAX_TASKS]` with
  `MAX_TASKS` = 4. Adding a third program past the ceiling meant editing the
  constant, and there was nowhere to allocate a `task_t` from because the frame
  allocator hands out whole pages, not arbitrary-size objects.
- The two user stacks were hard-coded: `TASK0_STACK_TOP` = `0x700000` and
  `TASK1_STACK_TOP` = `0x800000`, carving the single PG_USER stack page (PD[3],
  `0x600000`-`0x800000`) in half by hand. A caller had to know a magic address
  per task and pass it to `task_create`.

[Decision 0010](0010-kernel-heap-ported-from-p5.md) added a kernel heap
(`kmalloc`/`kfree`). That retires the first excuse: a `task_t` can now be
heap-allocated. The stacks are a separate problem with a hard constraint (below)
that the heap alone does not solve.

## Decision

Remove both fixed limits, keeping the switching logic in `schedule()` untouched.

- **Heap-allocate the task structs.** Replace the fixed `task_t[MAX_TASKS]` array
  with a pointer array `task_t *tasks[MAX_TASKS_LIMIT]`, each entry `kmalloc`'d in
  `task_create`. A flat pointer array (rather than a linked list) was chosen
  because it keeps `schedule()`'s O(1) round-robin indexing a mechanical `.`
  becomes `->` change rather than a rewrite, which matters because the switching
  logic is the one part that must not change. `MAX_TASKS_LIMIT` (64) is an
  arbitrary, generous cap on the bookkeeping array, not a storage ceiling: the
  structs live on the heap. If `kmalloc` returns NULL, `task_create` returns -1,
  the same failure contract as the old "table full".

- **Bump-allocate user stacks from the user region.** A user task's stack must be
  reachable at CPL 3. The kernel heap hands out frame-pool pages, which have no
  PG_USER bit, so a `kmalloc`'d stack would page-fault the instant a ring-3 task
  pushed to it. User stacks therefore cannot go on the kernel heap. Instead a tiny
  bump allocator (`alloc_user_stack` in `kernel/scheduler.c`) hands out fixed-size
  slices of the one PG_USER stack region (PD[3], `0x600000`-`0x800000`). The slice
  size is a named constant, `USER_STACK_SIZE` = 256KB, which divides the 2MB
  region into 8 stacks. `task_create` no longer takes a `stack_top` parameter: it
  asks the allocator for the next slice and returns -1 if the region is exhausted.

- **A third program to exercise the dynamic path.** `user_program_c` (prints "C",
  same shape as A and B) is added and all three are created in `kernel_main`,
  proving the scheduler now runs more than the old hardcoded two, each on its own
  dynamically-allocated stack.

## Consequences

- **The task-struct ceiling is gone.** The number of tasks is bounded only by the
  heap (and the arbitrary `MAX_TASKS_LIMIT` bookkeeping cap, trivially raised).
  More than two tasks now run: booted under QEMU the three programs interleave
  "ABC" forever.

- **The user-stack ceiling remains, and this change does not lift it.** All user
  stacks still share the one fixed PG_USER region (`0x600000`-`0x800000`), so at
  256KB each there are only 8 stacks, full stop. There are still no guard pages
  between slices, so a task that overflows its 256KB stack scribbles into its
  neighbour's, exactly as before but with smaller slices. Heap-allocating the
  task *struct* is orthogonal to this: the struct ceiling and the stack ceiling
  are different limits with different causes. The real fix is per-process paging,
  where each process gets its own address space and stacks stop competing for one
  shared region. A `TODO(per-process-paging)` marks this in `scheduler.c`.

- **User stacks are deliberately not on the kernel heap.** This is the load-
  bearing constraint of the whole change: the heap is the wrong tool for user
  stacks because its memory is not user-accessible. Anyone tempted to "just
  `kmalloc` the stack too" gets an immediate page fault on the first ring-3 push.

- **The switching logic is unchanged.** `schedule()` still saves the interrupted
  frame, round-robins to the next ready task, and overwrites the on-stack frame in
  place; the EOI-before-switch ordering and the startup-race guards are
  untouched. Only the indexing changed (`tasks[i].regs` became `tasks[i]->regs`,
  and the round-robin walk is bounded by `num_tasks` rather than the fixed
  `MAX_TASKS`).

- **Verified under QEMU.** With `-d int`, the syscall vector (`0x50`) fires from
  three distinct user RIPs (`0x40002b`, `0x400083`, `0x4000db`), each at `cpl=3`
  and on a distinct stack (`SP` near `0x640000`, `0x680000`, `0x6c0000`, matching
  the three 256KB slices), with the timer (`0x40`) still firing and zero page
  faults (`0x0E`) or GP faults (`0x0D`). A page fault on a stack push would have
  meant the allocator handed out an address outside the user region; none
  occurred.

## Related

- The heap this builds on: [0010](0010-kernel-heap-ported-from-p5.md).
- The scheduler whose switching logic is preserved:
  [0008](0008-round-robin-preemptive-scheduler.md).
- The user region the stacks are carved from:
  [0006](0006-user-mode-with-separate-pages.md).
- Reference pages: [../reference/scheduling.md](../reference/scheduling.md) and
  [../reference/memory-map.md](../reference/memory-map.md).
</content>
</invoke>
