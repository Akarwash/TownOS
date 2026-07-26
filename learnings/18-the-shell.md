# Chapter 18: The Shell

> Read chapters 16 (the filesystem) and 17 (loading a program) first. The shell
> is where everything you have built converges: the keyboard feeds it, the
> filesystem answers it, the loader runs what it launches.

## Where we are

You have a real operating system core. Processes with private memory. A scheduler.
A filesystem. A loader that runs programs off the disk.

But you cannot *use* it. It boots, it runs three programs that print letters, and
that is all it will ever do. There is no way to type a command. There is no way to
say "run this program" or "show me the files". The machine does exactly what it
was told at compile time and nothing else.

The shell is the thing that makes it usable. It is the part a person actually
talks to. And it is much smaller than the word suggests.

## What a shell actually is

A shell is a loop that does three things, forever:

1. **Read** a line the person typed.
2. **Match** the first word against a list of commands.
3. **Do** whatever that command says, then loop back.

Read, match, do. That is the whole thing. It is the same shape as the kernel's
event loop, except instead of reacting to interrupts, it reacts to words.

In shape:

```
forever:
    print a prompt
    line = read a line of input
    word, rest = split the line into the first word and the remainder
    match word against the command list:
        "list"   -> show the files
        "read"   -> print the file named in rest
        "run"    -> load and start the program named in rest
        "help"   -> print the command list
        "clear"  -> wipe the screen
        "return" -> print rest back
        else     -> "unknown command"
```

That loop is the shell. A prompt, a line reader, a way to split a line, and a stack
of comparisons. Nothing clever.

The commands are yours to name. `list`, `read`, `run` are just words. The machine
has never heard of `ls` or `cat`; those are conventions from Unix, not laws. A
shell whose commands are `yo` and `peep` and `yeet` is exactly as valid. The
computer has no opinion.

## The shell is a user program

Here is the important shift, and it is the reason the shell needs new machinery.

In the old design, the shell was **part of the kernel**. It ran at ring 0, it
touched the keyboard hardware directly, it called kernel functions directly. That
worked, but it was not really a shell in the way a real OS means it. It was kernel
code that happened to read keys.

The real shell is a **user program**. A file on the disk, loaded like any other,
running at ring 3, fenced in. And a fenced-in program cannot do the things a shell
needs:

- It cannot read the keyboard. The keyboard ports are ring 0 only.
- It cannot read the disk. Same reason.
- It cannot start a program. Only the kernel builds address spaces.

So the shell has to *ask* the kernel for every one of those, through system calls.
This is the whole point of building it as a user program: it proves the system
call boundary is real and complete. If a program locked in ring 3 can run an
interactive shell using nothing but syscalls, then the boundary works.

So the shell needs three new syscalls, on top of the write and exit it already
has.

## Syscall 1: read a key, and the buffer behind it

This is the one with a genuinely new idea in it, so it gets the most space.

The shell wants to read what you type. But think about *when* things happen.

The keyboard interrupt fires **whenever you press a key**. Unpredictable. The shell
asks for a key **when it is ready to read one**. These two moments do not line up.
You might press three keys while the shell is busy running a command. Where do
those three keys go?

If there is nowhere to put them, they are lost. The interrupt fires, nobody is
asking right now, the key vanishes.

### The buffer

The fix is a small holding area between the interrupt and the shell. The interrupt
**drops keys in**. The shell **takes keys out**. They no longer have to happen at
the same instant.

This shape has a name: **producer and consumer**. One side produces (the interrupt
makes keys), the other consumes (the shell reads them), and the buffer lets each
run at its own pace.

### The ring

The buffer is a small array with two markers:

- a **write** marker: where the interrupt drops the next key
- a **read** marker: where the shell takes the next key

```
slots:  [ h ][ i ][   ][   ][   ][   ][   ][   ]
              ^read           ^write
```

Key pressed: put it at the write marker, move write forward.
Shell asks: hand back the key at the read marker, move read forward.

When a marker runs off the end of the array, it **wraps back to slot 0**. That is
why it is called a **ring** (or a circular buffer): the two markers chase each
other around a loop, forever.

The gap between read and write is "keys waiting to be read".

- **read equals write**: the buffer is empty, nothing waiting.
- **write catches up to read from behind**: the buffer is full, and new keys get
  dropped (you typed faster than anything read, rare for a human).

### The two halves

**The interrupt (producer)** stays tiny and fast. It does not process the key, it
does not echo it, it just drops it in the ring and returns:

```
key pressed
-> put the character at the write marker
-> advance write (wrap if needed)
```

An interrupt handler must be short, because while it runs, other interrupts are
held off. Doing real work here would be a mistake. Drop and go.

**The read-key syscall (consumer)**:

```
if read equals write:   the buffer is empty
    return "nothing"
else:
    grab the character at the read marker
    advance read (wrap if needed)
    return the character
```

### The one real decision: what happens when it is empty

The shell asks for a key, but you have not typed anything. Two choices:

- **Block:** the shell waits until a key arrives. Nicer for the shell (it always
  gets a key), but "wait" is hard. The shell would sit there spinning, and with a
  simple scheduler that either wastes its whole time slice or needs a "put this
  task to sleep until a key comes" mechanism that does not exist yet.
- **Return nothing:** the syscall immediately says "no key right now", and the
  shell loops and asks again. Simpler to build. The shell busy-waits (asks over and
  over), which is wasteful, but the scheduler interleaves the other programs in
  between anyway, so it does not freeze the machine.

For a first shell, return-nothing is the honest simple choice. Real systems block
and sleep the task, which uses zero CPU while waiting. That needs task-sleeping,
which is its own feature. Note it and move on.

## Syscall 2: list a directory

The shell cannot read the disk (ring 3, no ports). So it asks the kernel: "walk
the root directory and hand me the file names."

The kernel does the FAT32 directory read it already knows how to do, and copies the
names into a buffer the shell provided.

The only wrinkle is one you have met before: the shell hands the kernel a **buffer
address**, and that address is untrusted. The kernel must bounds-check that the
buffer lies inside the shell's own memory before writing names into it. Same
principle as the syscall pointer check and the loader's segment check: any time
untrusted input names an address, bound it.

## Syscall 3: run a program

The shell says "run HELLO.ELF". The kernel calls the loader you already built
(`task_create_from_file`), a new process is created and joins the scheduler, and it
starts interleaving with everything else.

This is the smallest of the three. It is your existing loader behind a doorbell.
The shell keeps running; the launched program runs alongside it, because that is
what the scheduler does.

Notice the shape of all three: two of them (list, run) are just *exposing something
the kernel already does* to a user program. Only the key buffer is genuinely new.
That is what a maturing kernel looks like: new features become "let a user program
ask for the thing the kernel can already do".

## Splitting the line: your own tokenizer

The shell reads a whole line, like `read hello`. It needs the first word (`read`,
the command) and the rest (`hello`, the argument). Splitting a line into words is
called **tokenizing**.

The C library has a function for this called `strtok`, but the library does not
exist in a freestanding kernel, so you write your own. It is about a dozen lines,
and writing it means you understand exactly what it does, which is worth more than
the dozen lines cost.

### How it works: shred in place

The trick, and it is the same trick `strtok` uses, is that you do **not copy** the
words out. You destroy the original line in place and return pointers into it.

Take `read hello`. Find the space, **overwrite it with a string terminator**
(a zero byte), and now the buffer holds two separate strings back to back:

```
before:  r e a d _ h e l l o
after:   r e a d 0 h e l l o
         ^first    ^second
```

The zero byte in the middle ends the first word, so a pointer to the start reads as
`read`. A pointer just past the zero reads as `hello`. You made two strings out of
one by dropping a terminator into the gap. No copying, no second buffer.

### The rhythm

Each call to your tokenizer does three things:

1. **Skip any spaces** at the current position. There might be several between
   words, or leading spaces. Skipping them first is what stops double-spaces from
   producing empty words.
2. **Walk forward to the next space** (or the end of the line), and drop a zero
   there to end this word.
3. **Remember where you stopped**, so the next call resumes after it. Return where
   the word started.

```
next_word(position):
    skip spaces at position
    if at the end: return nothing
    mark the start
    walk to the next space or the end
    drop a zero there
    move position past it
    return the start
```

### One design choice worth making deliberately

`strtok` remembers where it left off in a **hidden global variable**. That is why,
in the classic version, later calls pass `NULL`: they mean "keep going in the string
you are already chewing on". The hidden global is also a notorious bug source,
because it means you can only tokenize one string at a time in the whole program.

Do it cleaner: pass the position explicitly, as a pointer the caller holds. No
hidden state, no one-at-a-time restriction. The C library's own fix for this is
called `strtok_r` (the "r" is for reentrant), and it takes exactly such a pointer.
You are writing the good version from the start.

## Putting it together

The shell, end to end:

1. Print a prompt.
2. Loop calling the read-key syscall, collecting characters into a line buffer,
   until the key is Enter. Echo each key with the write syscall so the person sees
   what they type. (Return-nothing means this loop spins when no key is ready,
   which is fine.)
3. Tokenize the line into words.
4. Compare the first word against the command list.
5. Run the matching command: `list` calls the directory syscall and prints the
   names; `read` reads a file and prints it; `run` calls the run syscall; the rest
   are local.
6. Go back to step 1.

Every command is either a local bit of printing or a syscall you built. The shell
itself is just the glue.

## What this still is not

- **No arguments passed to launched programs.** `run hello` starts `hello`, but it
  cannot pass `hello` any arguments, because the loader does not set up argv yet.
- **No pipes or redirection.** Real shells connect one program's output to
  another's input (`a | b`) or to a file (`a > out`). That needs a way to redirect
  where a program's output goes, which needs the output to be a thing you can point
  somewhere, which you do not have.
- **No job control.** No background programs, no stopping a running one. The
  scheduler does not support removing a task yet.
- **No history, no editing, no tab completion.** These are all conveniences layered
  on top of the read loop. None are hard; none are essential.
- **Return-nothing, not blocking.** The shell busy-waits for keys instead of
  sleeping. Real shells sleep the task and wake on a keypress.

Each is a real feature. None is needed for a shell you can actually type commands
into.

## Exercises

1. You press three keys while the shell is busy running a command. Explain, in
   terms of the ring buffer, why they are not lost, and what exactly happens to
   them.
2. The read marker and the write marker are equal. What does that mean, and how is
   it different from the buffer being full?
3. Why must the keyboard interrupt handler only drop the key in the buffer and
   return, rather than processing the key itself?
4. Your tokenizer is given the line `  run   hello  ` with extra spaces
   everywhere. Walk through what it returns on each call and why no empty words
   appear.
5. Why does the tokenizer overwrite spaces with a zero byte instead of copying each
   word into a fresh buffer? What does this cost, and what does it save?
6. The shell is a ring-3 program. List every syscall it must make to run the
   command `read notes.txt`, in order, from the keystroke that starts it to the
   file appearing on screen.
7. The "return nothing when empty" choice makes the shell busy-wait. Explain why
   this does not freeze the whole machine, and what a real OS does instead.
