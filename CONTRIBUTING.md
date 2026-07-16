# Contributing to MiniOS

MiniOS is a solo learning project, but it follows consistent conventions so the
history stays legible and the hand-written parts stay hand-written. This file
records those conventions.

## Workflow

- **A new branch per working session.** Each session's work happens on its own
  branch (for example a dated branch), not directly on `main`.
- **Small, logically-scoped commits.** One coherent change per commit, with a
  message that explains the why, not just the what.

## Hand-written vs generated code

Some code in MiniOS is deliberately written by hand and must not be
machine-generated, because writing it is the point of the exercise and because it
is load-bearing enough that understanding every line matters. This applies to the
privileged and conceptually load-bearing paths:

- the boot path and the 32 to 64 long-mode climb (`boot/boot.asm`),
- the GDT and TSS (`kernel/gdt.c`, `kernel/gdt.h`, `kernel/gdt_flush.asm`),
- the IDT (`kernel/idt.c`),
- the ISR/IRQ entry stubs (`kernel/isr_stubs.asm`).

Tooling may assist with mechanical conversions, documentation, and portable C, but
the paths above are reasoned through and written by hand.

## The `TODO(long-mode)` marker

Unfinished hand-written work is marked with a `TODO(long-mode)` comment at the
exact spot that needs implementing, describing what belongs there and why it is
not generated. Grep for `TODO(long-mode)` to find every remaining stub. Do not
remove one of these markers without actually implementing the code it guards.

## Code style

- Match the surrounding file. C and assembly both use `;`- or `//`-style comment
  banners for section headers.
- Prefer named constants (`#define` or `equ`) over inline magic numbers,
  especially for bit flags.
- Comment the why, not the what. Several files double as learning material, so a
  comment should explain the reasoning a reader would otherwise have to
  reconstruct.
- Keep it readable over clever.

## Documentation

Two separate bodies of documentation, and they must not blur:

- **`docs/`** is factual project documentation, derived from the actual source.
  If a fact is not in the code, do not state it; if something is pending, say so.
- **`learnings/`** is conceptual teaching material about how operating systems
  work in general.

When they overlap on a fact, `docs/` is authoritative. Prose style in both:
direct and plain, no filler, no em dashes (use commas or parentheses). Every
cross-reference is a working relative link.

Record notable changes in [CHANGELOG.md](CHANGELOG.md) (Keep a Changelog format),
and capture load-bearing decisions as an ADR in
[docs/decisions/](docs/decisions/).
