# Long mode and paging

This chapter will teach why a modern x86 CPU does not simply start in its most
capable state but has to be walked up to it in stages, and why every address a
program touches is a virtual address that the hardware translates to physical RAM
rather than using directly. The concept, not the register-by-register climb: what
paging buys an operating system, and why the ability to make an address mean
different memory at different times is the foundation everything above it stands
on.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the boot sequence](../docs/reference/boot-sequence.md), [paging](../docs/reference/paging.md), and [the memory map](../docs/reference/memory-map.md).

Reference pages:
[../docs/reference/boot-sequence.md](../docs/reference/boot-sequence.md),
[../docs/reference/paging.md](../docs/reference/paging.md),
[../docs/reference/memory-map.md](../docs/reference/memory-map.md).
