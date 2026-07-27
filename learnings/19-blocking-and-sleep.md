# Blocking and sleep

This chapter will teach what it means for a program to wait, and why a kernel that
cannot express "this task has nothing to do" burns a whole CPU proving it. The
concept, not the rewind trick: why waiting is a *state* rather than a loop, why
the thing that causes an event is the thing that must wake the waiters, why a
sleeping machine has to be able to halt and still be woken, and why almost
everything a real kernel does (waiting on a child, on a pipe, on a disk, on a
lock) is this one shape wearing different clothes.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the blocking reference](../docs/reference/blocking.md).

Reference page: [../docs/reference/blocking.md](../docs/reference/blocking.md).
