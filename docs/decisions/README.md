# Architecture decision records

One file per decision, numbered in the order they were made. Each records what was
decided, what the situation was at the time, what else was considered, and what it
cost. They are the reasoning behind the code, written down while it was still fresh.

## The rule: an ADR body is read-only

**Do not edit the Context, Decision, Alternatives or Consequences of an ADR that
has already been written. Only the Status section changes.**

This applies even — especially — when the body is wrong. If an ADR says the kernel
copies a linked ring-3 image into every task and the kernel does no such thing any
more, that is not a bug in the ADR. That is the ADR doing its job.

### Why

An ADR is not documentation of how the system works. That is what
[`docs/reference/`](../reference/) is for, and those pages should be corrected the
moment they go stale. An ADR is a record of **a decision, at a moment, given what
was known then**. Its value is entirely in being that record.

Editing the body destroys the thing being recorded, and does it invisibly:

- **It rewrites the past into something that was never true.** A body updated to
  match today's code claims that today's design was decided back then, with today's
  constraints in view. It was not. The next person reads a confident account of
  reasoning that never happened.
- **It deletes the reason the next decision was made.** ADR 0015 (per-file ELF
  loading) only makes sense against what ADR 0012 actually said. Tidy 0012 into
  agreement with the current code and 0015 becomes a solution to a problem no
  document admits existed.
- **It hides the trail.** The interesting question about a design is rarely "what is
  it" — you can read the code for that. It is "why is it this and not the obvious
  thing", and the answer is usually a sequence of decisions each of which looked
  right at the time. A set of ADRs that all agree with the current code has no
  sequence in it. It is a snapshot wearing a timestamp.
- **A wrong ADR is honest; a retrofitted one is not.** "This is what we thought in
  March, and here is what changed our minds" is useful. "This is what we have always
  thought" is a claim nobody can check.

The cost of the rule is that a body can mislead a reader who does not check the
Status line first. That cost is real, and the Status line is how it is paid.

### What to do instead

**Add to the Status section.** Keep `Accepted.`, then say what has been superseded,
by which ADR, and in what respect. Be specific about the *part* — most supersession
is partial, and "superseded by 0015" on an ADR whose core decision still stands is
its own kind of lie. For example:

```markdown
## Status

Accepted. Partly superseded — the decision stands, one detail of the body does not.

- **How the user half is filled** was superseded by [0015](0015-elf-program-loading.md).
  The body describes `task_create` copying the whole linked ring-3 image into every
  task. Programs are now separate ELF files and `elf_load_file` maps one program's
  own `PT_LOAD` segments.

The private tree per task, the by-value kernel-half clone and the CR3 switch are
unchanged. See [reference/paging.md](../reference/paging.md) for the current state.
```

Three things that entry does, all of them deliberate: it names what changed rather
than condemning the whole ADR, it points at the ADR that changed it so the trail can
be followed forward, and it points at the reference page so a reader who wants
today's truth can stop reading history.

**Correct the reference pages freely.** [`docs/reference/`](../reference/) describes
the system as it is. It has no historical duty and a stale page there is simply a
bug. Same for code comments, `docs/architecture.md`, and `docs/project-status.md`.

**Write a new ADR when the decision itself changes.** If the answer is different now
and the reasons are worth recording, that is the next number in the sequence, not an
edit to an old one.

## Where things are

| | |
|---|---|
| `docs/decisions/` | Why, at the time. Bodies frozen; Status lines maintained. |
| [`docs/reference/`](../reference/) | How it works today. Correct on sight. |
| [`learnings/`](../../learnings/) | Notes written while learning a topic. Also a record of a moment, also not maintained as truth. |
| [`CHANGELOG.md`](../../CHANGELOG.md) | What changed, in order. Append-only for the same reason. |
