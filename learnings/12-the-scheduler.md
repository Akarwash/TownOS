# The scheduler

This chapter will teach how a single CPU appears to run several programs at once
by rapidly switching between them, what a program's "context" is that must be
saved and restored across a switch, and what it means for the kernel to take the
CPU back from a program that never volunteered to give it up. The concept, not the
frame-swapping trick: why time-sharing is the illusion at the heart of a
multitasking operating system, and what preemption costs and buys.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the scheduling reference](../docs/reference/scheduling.md).

Reference page: [../docs/reference/scheduling.md](../docs/reference/scheduling.md).
