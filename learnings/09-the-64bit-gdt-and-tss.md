# The 64-bit GDT and the TSS

This chapter will teach why a 64-bit kernel still keeps a table that describes
memory segments and privilege levels even though it has stopped using segments to
divide memory, and what the task state segment is for. The concept, not the
descriptor bit layout: what the hardware still insists on being told before it
will run code at one privilege level and let it trap into another, and why that
leftover table is the thing that makes the kernel/user boundary possible at all.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the GDT and TSS reference](../docs/reference/gdt.md).

Reference page: [../docs/reference/gdt.md](../docs/reference/gdt.md).
