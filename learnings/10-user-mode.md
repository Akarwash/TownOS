# User mode

This chapter will teach the split between privileged kernel code that may touch
anything and unprivileged program code that may not, why the hardware itself
enforces that split rather than trusting software to behave, and what it actually
means for the same CPU to run at a lower privilege level. The concept, not the
mechanics of the drop: why an operating system deliberately gives up power before
running a program, and what the program can and cannot do once it is down there.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the user-mode reference](../docs/reference/user-mode.md).

Reference page: [../docs/reference/user-mode.md](../docs/reference/user-mode.md).
