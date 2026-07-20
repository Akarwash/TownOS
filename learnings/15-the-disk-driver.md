# The disk driver

This chapter will teach how a disk presents itself to software as a flat array of
fixed-size numbered blocks, how the CPU moves those blocks to and from memory, and
where the line falls between a raw block device that only moves the block it is
told to and a filesystem that decides which block holds what. The concept, not the
ATA port sequence: why storage is addressed by number rather than by name, what
"persistent" costs, and why naming and free-space tracking are a separate layer.

> STATUS: stub. Teaching content to be written by hand. For the factual/reference version now, see [the disk reference](../docs/reference/disk.md).

Reference page: [../docs/reference/disk.md](../docs/reference/disk.md).
