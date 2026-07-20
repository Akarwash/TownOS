# Per-process paging

This chapter will teach how giving each program its own map from virtual addresses
to physical memory isolates programs from one another, so that two can use the
very same address for different memory and neither can see or corrupt the other,
and what the phrase "address space" really means. The concept, not the by-value
clone: why isolation is the payoff of paging, why the kernel must still be present
in every program's map, and how a stray pointer becomes a fault instead of quiet
corruption.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the paging reference](../docs/reference/paging.md).

Reference page: [../docs/reference/paging.md](../docs/reference/paging.md).
