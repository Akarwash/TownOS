# System calls

This chapter will teach how an unprivileged program asks the kernel to do
something it is not allowed to do for itself, and why that request has to go
through one deliberately narrow, controlled doorway rather than a free function
call. The concept, not the gate wiring: why the boundary between a program and the
kernel is crossed by asking rather than by jumping, and how the kernel stays in
control of what is asked of it.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the syscalls reference](../docs/reference/syscalls.md).

Reference page: [../docs/reference/syscalls.md](../docs/reference/syscalls.md).
