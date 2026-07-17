# 2. Protected Mode and the GDT

**Source files:** `kernel/gdt.c`, `kernel/gdt.h`, `kernel/gdt_flush.asm`

> **Note: this chapter describes the original 32-bit design.** MiniOS is now an
> x86-64 long-mode kernel. The concepts below still hold, but the specifics have
> moved on: the flat three-entry 32-bit GDT is now a seven-slot 64-bit GDT with a
> TSS, and `gdt_flush` reloads CS with a far return (`retfq`) rather than the
> 32-bit far jump shown here. For what actually runs today, see
> [../docs/reference/gdt.md](../docs/reference/gdt.md). This chapter is kept as
> personal learning material and is intentionally not rewritten.

This chapter is about how the CPU *describes and protects memory* — and why an
OS's very first real job is to hand the CPU a table explaining what memory means.

## The big idea

A modern CPU refuses to run in its powerful mode until you give it a **map of
memory permissions**. That map answers, for every memory access: *Is this region
code or data? Who is allowed to touch it — the kernel or a user program? How big
is it?* On x86 this map is the **Global Descriptor Table (GDT)**, and building it
is one of the first things any x86 kernel does.

The general principle transfers everywhere: before a CPU will let you use memory
safely, the OS must *declare the rules*, and the hardware then enforces them on
every single instruction with zero per-access software cost. Declaring rules once
and letting silicon enforce them forever is the central trick of OS protection.

## How the hardware forces this: segmentation

x86 has two memory-protection mechanisms stacked on top of each other:

1. **Segmentation** (older, via the GDT) — the topic of this chapter.
2. **Paging** (newer, more powerful) — chapter 5.

Segmentation comes from the 8086, where memory was addressed as
`segment:offset`. In 16-bit real mode a segment register just scales an address.
But in **32-bit protected mode**, segment registers no longer hold addresses —
they hold **selectors**: indices into the GDT. Each GDT entry (a *descriptor*)
says where a segment starts (`base`), how big it is (`limit`), and what is allowed
(`access` flags).

Every memory access implicitly goes through a segment register:

- Instruction fetches use **CS** (code segment).
- Most data access uses **DS** (data segment).
- Stack access uses **SS** (stack segment).

So the CPU is *always* consulting the GDT, whether you think about it or not. If
the GDT is wrong or missing, the very next memory access faults and the machine
triple-faults (resets). That is why the GDT must be correct before almost
anything else runs.

### Privilege rings

Descriptors also encode a **privilege level** (0–3), the famous **rings**:

- **Ring 0** = kernel. Can do anything.
- **Ring 3** = user programs. Fenced in.
- Rings 1 and 2 exist but almost nobody uses them.

The CPU checks the ring on every access and every privileged instruction. This is
the hardware foundation of the kernel/user split from chapter 0. This chapter
described MiniOS as ring-0-only; that is no longer strictly true — it now drops to
ring 3 to run a small demonstration program in its own pages, which is exactly
what the user code/data descriptors declared here are for. The mechanism is in
[`../docs/reference/user-mode.md`](../docs/reference/user-mode.md). The concept
below is unchanged: descriptors still have to *declare* their ring, because the
field is not optional.

## The "flat" memory model

Segmentation is powerful but awkward: you would have to manage many segments with
different bases and limits. Almost every modern OS (Linux, Windows) sidesteps it
with a trick called the **flat memory model**:

> Define segments that all start at base `0` and span the entire 4 GB address
> space. Now `segment:offset` is just `offset` — a plain 32-bit address — and
> segmentation effectively "disappears." Real protection is then done with paging
> instead.

MiniOS uses exactly this flat model. Its GDT has just three entries:

| Index | Selector | Purpose | Base | Limit | Access |
|-------|----------|---------|------|-------|--------|
| 0 | 0x00 | Null descriptor (required, never used) | 0 | 0 | 0x00 |
| 1 | 0x08 | Kernel code | 0 | 0xFFFFF | 0x9A |
| 2 | 0x10 | Kernel data | 0 | 0xFFFFF | 0x92 |

The **null descriptor** at index 0 is mandatory: the CPU treats selector 0 as "no
segment," and any attempt to actually use it faults. It is a safety tripwire
against uninitialised segment registers.

The selectors `0x08` and `0x10` are not arbitrary — a selector is
`index << 3` (the low 3 bits hold ring and table-type). Index 1 → `0x08`, index 2
→ `0x10`. You will see `0x08` and `0x10` hardcoded all over the kernel (in the IDT
entries and the ISR stubs); now you know they simply mean "kernel code segment"
and "kernel data segment."

## Reading the magic numbers

The `access` and `granularity` bytes look like noise until you decode them.

`0x9A` = `1001 1010`:
- `1` present (this descriptor is valid)
- `00` ring 0
- `1` this is a code/data segment (not a system segment)
- `1` executable → it is **code**
- `0` conforming bit
- `1` readable
- `0` accessed (CPU sets this)

`0x92` = `1001 0010`: same but the executable bit is `0`, so it is **data**, and
the `1` in that position means writable. Code segments are read/execute; data
segments are read/write. That difference is the whole point of having two entries.

`0xCF` granularity = `1100 1111`:
- `1` granularity bit: interpret the limit in **4 KB pages**, not bytes. With
  limit `0xFFFFF` that is `0xFFFFF * 4 KB = 4 GB`. This is what makes the segment
  cover all of memory.
- `1` 32-bit segment (default operand size).
- The low nibble `F` is the top 4 bits of the 20-bit limit.

## How MiniOS builds and loads it

`gdt.c` defines the descriptor struct (note `__attribute__((packed))` — the
hardware layout must be exact, with no compiler padding; see the glossary) and a
helper `gdt_set_entry` that splits `base` and `limit` across the descriptor's
awkwardly-scattered fields (a historical layout inherited from the 80286).

`gdt_init()` fills the three entries, then calls `gdt_flush` with the address of a
`gdt_ptr` structure (a 16-bit limit + 32-bit base — the operand the `lgdt`
instruction expects).

The actual load must be assembly, in `gdt_flush.asm`:

```asm
gdt_flush:
    mov eax, [esp + 4]   ; the gdt_ptr address passed from C
    lgdt [eax]           ; tell the CPU "here is the new GDT"

    mov ax, 0x10         ; kernel data selector
    mov ds, ax           ; reload every data segment register
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush      ; far jump reloads CS with the kernel code selector
.flush:
    ret
```

Two subtleties worth internalising:

1. **Loading the GDT is not enough.** The segment registers still cache the *old*
   descriptors. You must reload them. Data segments are reloaded with plain
   `mov`s, but **CS cannot be set with `mov`** — the only way to change CS is a
   **far jump**. `jmp 0x08:.flush` jumps to the very next line but, as a side
   effect, loads `0x08` into CS. That far jump is the moment the CPU truly starts
   using your new code segment.

2. This is a recurring OS pattern: *a table describes something, a register points
   at the table, and a special instruction commits the change.* You will see it
   again with the IDT (`lidt`) in chapter 3 and page tables (`mov cr3`) in
   chapter 5.

## Going further

- **Linux and Windows** set up a flat GDT almost identical to this one and then do
  essentially all memory management with **paging** (chapter 5). Segmentation is
  legacy; the flat GDT exists mostly to satisfy the hardware and to define the
  ring-0/ring-3 code and data segments needed for system calls.
- **64-bit long mode** finally retires most of segmentation: base and limit are
  ignored for CS/DS/SS, and the GDT shrinks to little more than "here are the
  privilege levels." The flat model became mandatory rather than a trick.
- The **TSS** (Task State Segment) is another descriptor type you would add to the
  GDT once you introduce user mode, so the CPU knows which kernel stack to switch
  to on an interrupt from ring 3.

### Exercises

1. Decode the access byte `0xFA`. What kind of segment is it, and at what ring?
   (This is the descriptor a real OS would add for *user* code.)
2. Why is a far jump required to reload CS but a plain `mov` works for DS? What
   does the CPU pipeline have to do differently?
3. If you removed the null descriptor and made index 0 the kernel code segment,
   what would break, and why is the current design safer?
