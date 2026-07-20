# The heap

This chapter will teach how a kernel hands out memory in whatever sizes are asked
for, built on top of a lower layer that only deals in fixed-size pages, and what
an allocator has to remember so that memory handed back can be found, merged, and
handed out again. The concept, not the boundary-tag layout: why dynamic allocation
is a bookkeeping problem, what fragmentation is, and why freeing is harder than
allocating.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the heap reference](../docs/reference/heap.md).

Reference page: [../docs/reference/heap.md](../docs/reference/heap.md).
