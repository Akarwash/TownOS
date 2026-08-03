// The TownOS interactive shell, as a ring-3 program.
//
// This is the capstone of the syscall boundary. The shell is a fully fenced-in
// user program: it is loaded off the disk like any other (as SHELL.ELF), runs at
// CPL 3 in its own address space, and cannot touch the keyboard, the screen, the
// filesystem, or the loader except through `int 0x50`. Everything it does, reading
// a key, echoing it, listing files, printing a file, launching a program, it does
// with nothing but the syscalls in userlib.h. That it works at all is the proof
// that the boundary is complete.
//
// Built and linked exactly like the test fixtures in user/tests/ (see the
// SHELL.ELF rule in the Makefile
// and user/user.ld), copied onto the FAT32 image as SHELL.ELF, and launched by
// kernel_main. See docs/reference/shell.md.

#include "userlib.h"

// The line buffer is fixed. If it fills, further printable characters are DROPPED
// rather than accepted, so a long line cannot overflow it; one slot is reserved
// for the terminating '\0'.
#define SHELL_LINE_MAX   128

// Scratch for SYS_LIST. Big enough for the handful of 8.3 names on the disk, one
// per line; if the kernel had more names than fit it would drop the overflow.
#define SHELL_LIST_MAX   512

// Scratch for SYS_READFILE. Sized past the largest test file (BIG.TXT is 16384
// bytes) so `read` can print it whole. One byte is reserved for the '\0' the shell
// appends before printing, so SYS_READFILE is asked for at most SHELL_FILE_MAX - 1.
#define SHELL_FILE_MAX   32768

// `clear` scrolls the screen by printing a screenful of newlines. There is no
// clear-screen syscall (that would be a fifth syscall for a cosmetic command), and
// the screen is 25 rows, so 25 newlines pushes everything off. The prompt then
// reprints at the bottom.
#define SHELL_CLEAR_LINES 25

// Static (in .bss, inside the ring-3 region), not on the stack: the file buffer is
// large, and keeping these off the 256KB user stack leaves it for call frames.
static char line[SHELL_LINE_MAX];
static char list_buf[SHELL_LIST_MAX];
static char file_buf[SHELL_FILE_MAX];

// The value SYS_LIST / SYS_RUN / SYS_READFILE return on failure. The syscalls hand
// back (unsigned long)-1; name it here so the checks below read as intent.
#define SYS_FAIL  ((unsigned long)-1)

// A minimal string compare: the freestanding user program has no libc, so this is
// hand-rolled. Returns 1 when the two NUL-terminated strings are equal.
static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;   // equal only if both reached '\0' at the same spot
}

// Length of a NUL-terminated string. `write` needs it to tell the kernel how many
// bytes of the line to store, and there is no libc strlen to reach for.
static unsigned long str_len(const char *s) {
    unsigned long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

// Is `name` expressible as an 8.3 name: a base of 1..8 characters, optionally a
// dot and an extension of 0..3? This mirrors the kernel's name_to_83 acceptance
// rule (lengths only; it does not judge individual characters), and it lives here
// so the shell can tell the user their name is the problem BEFORE the syscall,
// distinguishing the one failure that is their fault and fixable by retyping from
// every other reason a write or delete can fail. Never mangle silently.
static int name_is_83(const char *name) {
    int base = 0;
    int i = 0;
    while (name[i] != '\0' && name[i] != '.') {
        if (++base > 8) {
            return 0;   // base too long
        }
        i++;
    }
    if (base == 0) {
        return 0;       // no base name at all
    }
    if (name[i] == '.') {
        i++;
        int ext = 0;
        while (name[i] != '\0') {
            if (++ext > 3) {
                return 0;   // extension too long
            }
            i++;
        }
    }
    return 1;
}

// Print a small non-negative number in decimal. There is no printf and no libc
// here, and the only way out is sys_write, which takes a string: so the digits
// have to be built by hand. Only exit statuses (0..255) go through this, but the
// buffer is sized for the full range of an unsigned long anyway, because a buffer
// sized for exactly the values you expect today is how this kind of helper gets
// overflowed tomorrow.
//
// The digits come out least-significant first, so they are written backwards from
// the end of the buffer and the pointer to the first one is returned. Note the
// do/while: a plain `while (value)` would print nothing at all for zero, which is
// the single most common status there is.
static void print_uint(unsigned long value) {
    char buf[21];              // 20 digits is the most an unsigned 64-bit value needs, plus '\0'
    char *p = &buf[20];
    *p = '\0';

    do {
        *--p = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    sys_print(p);
}

static void print_help(void) {
    // The command names are TownOS's own and deliberately not the Unix ones.
    sys_print("commands:\n");
    sys_print("  list                 list files in the root directory\n");
    sys_print("  read <file>          print a file's contents\n");
    sys_print("  write <file> <text>  write the rest of the line to a file (creates/replaces)\n");
    sys_print("  delete <file>        delete a file\n");
    sys_print("  free                 how many clusters on the volume are free\n");
    sys_print("  run <file>           run a program and wait for it to finish\n");
    sys_print("  help                 show this list\n");
    sys_print("  clear                clear the screen\n");
    sys_print("  return <text>        print the text back\n");
}

static void cmd_list(void) {
    if (sys_list(list_buf, sizeof(list_buf)) == SYS_FAIL) {
        sys_print("list: could not read the directory\n");
        return;
    }
    // The kernel filled list_buf with newline-separated names, NUL-terminated.
    sys_print(list_buf);
}

static void cmd_read(char *name) {
    if (name == (char *)0) {
        sys_print("read: missing filename\n");
        return;
    }

    // STAT FIRST, then decide. Before this, a missing file, a file too big for the
    // buffer, and a disk error all printed the same "cannot read" line, so a user
    // could not tell which had happened. Asking for the size up front splits the
    // two common cases off with their own messages, and only a genuine read error
    // falls through to the old line.
    unsigned long size = 0;
    if (sys_stat(name, &size) == SYS_FAIL) {
        sys_print("read: no such file: ");
        sys_print(name);
        sys_print("\n");
        return;
    }

    // The buffer holds SHELL_FILE_MAX - 1 bytes of file content (one byte is kept
    // for the NUL appended below before printing). A file larger than that is
    // refused, now WITH BOTH NUMBERS so the reason is unambiguous. There is
    // deliberately no partial read: `read` still delivers the whole file or none of
    // it, it just says why when it declines. Showing a prefix would need an offset
    // argument on SYS_READFILE, which is a rung of its own; see
    // docs/decisions/0021-sys-stat.md. This also retires the old unreachable
    // "showing the first N bytes" notice, which only ever fired for a file of
    // exactly the buffer size, which is complete (TODO(read-truncation), now gone).
    if (size > sizeof(file_buf) - 1) {
        sys_print("read: ");
        sys_print(name);
        sys_print(" is ");
        print_uint(size);
        sys_print(" bytes, the buffer holds ");
        print_uint(sizeof(file_buf) - 1);
        sys_print("\n");
        return;
    }

    // Small enough to fit. The size is known, so this is not the too-large case; a
    // failure here is a genuine disk or filesystem error, which the "cannot read"
    // line now names on its own.
    unsigned long n = sys_readfile(name, file_buf, sizeof(file_buf) - 1);
    if (n == SYS_FAIL) {
        sys_print("read: cannot read ");
        sys_print(name);
        sys_print("\n");
        return;
    }
    // File contents are raw and not NUL-terminated, so terminate before printing.
    file_buf[n] = '\0';
    sys_print(file_buf);
    sys_print("\n");   // the file may not end in a newline; keep the prompt tidy
}

// `write <file> <text>`: store the rest of the line, verbatim, as the file's
// contents. `content` is what next_token left pointing at after the filename: the
// untouched remainder of the line, so every space inside it is preserved and NO
// trailing newline is added — exactly what was typed becomes the file. An empty
// remainder writes a zero-length file. Single-cluster files only, in practice,
// since the line buffer caps a typed line well under one cluster; multi-cluster
// writing is exercised by user/tests/F.c, not by typing.
static void cmd_write(char *name, char *content) {
    if (name == (char *)0) {
        sys_print("write: missing filename\n");
        return;
    }
    if (!name_is_83(name)) {
        // The user's fault and fixable by retyping, so it is called out on its own
        // rather than lumped in with disk failures.
        sys_print("write: ");
        sys_print(name);
        sys_print(" is not an 8.3 name (max 8 chars, dot, 3 chars)\n");
        return;
    }
    if (sys_writefile(name, content, str_len(content)) != 0) {
        sys_print("write: could not write ");
        sys_print(name);
        sys_print("\n");
        return;
    }
}

// `free`: print how many clusters on the volume are free. The leak test leans on
// this: create and delete a file repeatedly and this number must return to exactly
// where it started, because a cluster stranded on any cycle would show up as the
// count drifting down. fat32_free_count recounts the whole FAT, so it is honest
// rather than a cached total that a leak could hide behind.
static void cmd_free(void) {
    print_uint(sys_freecount());
    sys_print(" clusters free\n");
}

// `delete <file>`: remove a file from the disk.
static void cmd_delete(char *name) {
    if (name == (char *)0) {
        sys_print("delete: missing filename\n");
        return;
    }
    if (!name_is_83(name)) {
        sys_print("delete: ");
        sys_print(name);
        sys_print(" is not an 8.3 name (max 8 chars, dot, 3 chars)\n");
        return;
    }
    if (sys_delete(name) != 0) {
        sys_print("delete: could not delete ");
        sys_print(name);
        sys_print("\n");
        return;
    }
}

static void cmd_run(char *name) {
    if (name == (char *)0) {
        sys_print("run: missing filename\n");
        return;
    }
    if (sys_run(name, -1, -1) == SYS_FAIL) {   // -1/-1: a plain run, fresh console, no pipe
        sys_print("run: could not start ");
        sys_print(name);
        sys_print("\n");
        return;
    }
    // The program is now a task of its own and its output interleaves with this
    // shell from the next timer tick on. Announce it before waiting, so the letters
    // that follow are visibly attributed to something that was started.
    sys_print("run: started ");
    sys_print(name);
    sys_print("\n");

    // WAIT FOR IT. This is what makes `run` feel like a command rather than a
    // detach: the prompt does not come back until the program is finished, because
    // this call blocks until it is. Costs no CPU while it waits (see sys_wait).
    //
    // THE CHILD MUST EXIT. There is no way to kill a task and there are no signals,
    // so if the program never calls sys_exit, this shell blocks here forever and the
    // only way back is a reboot. That is why every program in user/ has a bounded
    // loop.
    long status = sys_wait();

    // Any real status is 0..255 (the kernel masks it), so a negative return is the
    // error case and cannot be confused with a program that exited 255. It means the
    // kernel says this shell has no children: the program we just started must have
    // finished AND been reaped before we got here, which today cannot happen because
    // only this task reaps its own children. Report it rather than printing a status
    // that was never returned.
    if (status < 0) {
        sys_print("run: no child to wait for\n");
        return;
    }

    sys_print("run: ");
    sys_print(name);
    sys_print(" exited with status ");
    print_uint((unsigned long)status);
    sys_print("\n");
}

static void cmd_clear(void) {
    for (int i = 0; i < SHELL_CLEAR_LINES; i++) {
        sys_print("\n");
    }
}

// `return <text>`: echo the rest of the line. `rest` is what next_token left
// pointing at after the "return" token: the untouched remainder of the line, so
// internal spaces are preserved. Skip any extra separators between "return" and
// the text so `return   hi` prints `hi`, not `  hi`.
static void cmd_return(char *rest) {
    while (*rest == ' ') {
        rest++;
    }
    sys_print(rest);
    sys_print("\n");
}

// Read one line, building it a keystroke at a time. Returns with `line` holding a
// NUL-terminated string (without the newline). Echoes as it goes so the user sees
// the line forming.
static void read_line(void) {
    unsigned int len = 0;

    for (;;) {
        // Blocks until a key is actually available, so this loop turns exactly once
        // per keystroke and the shell costs nothing while the user is thinking. It
        // used to spin here, calling a non-blocking read over and over and burning
        // every slice it was given; the kernel now sleeps the task instead.
        char c = (char)sys_readkey();

        if (c == '\n') {
            sys_print("\n");   // echo the newline that ends the line
            break;
        }

        if (c == '\b') {
            // Backspace: drop the last character if there is one, and erase it on
            // screen (the screen driver's '\b' rubs out the glyph). Guarding on
            // len > 0 means backspace cannot chew back into the prompt.
            if (len > 0) {
                len--;
                sys_print("\b");
            }
            continue;
        }

        // A printable character. Append it only if the fixed buffer has room (one
        // slot reserved for '\0'); otherwise DROP it rather than overflow. Echo it
        // so the user sees what they type.
        if (len < SHELL_LINE_MAX - 1) {
            line[len++] = c;
            char echo[2] = { c, '\0' };
            sys_print(echo);
        }
    }

    line[len] = '\0';
}

// The entry point named by user.ld's ENTRY(_start). The loader takes the entry
// address from the ELF header, so this symbol only has to match the linker script.
void _start(void) {
    sys_print("TownOS shell. type 'help'.\n");

    for (;;) {
        sys_print("> ");
        read_line();

        // Tokenize IN PLACE: next_token shreds `line`, so the command and any
        // argument point straight into it. `pos` walks along the line.
        char *pos = line;
        char *cmd = next_token(&pos, ' ');

        if (cmd == (char *)0) {
            continue;   // empty line (or only spaces): reprint the prompt
        }

        if (str_eq(cmd, "list")) {
            cmd_list();
        } else if (str_eq(cmd, "read")) {
            cmd_read(next_token(&pos, ' '));
        } else if (str_eq(cmd, "write")) {
            // Extract the filename FIRST, then hand cmd_write the raw remainder as
            // the contents. Two statements, not one call: C leaves argument
            // evaluation order unspecified, and `pos` must be read only after
            // next_token has advanced it past the filename.
            char *wname = next_token(&pos, ' ');
            cmd_write(wname, pos);
        } else if (str_eq(cmd, "delete")) {
            cmd_delete(next_token(&pos, ' '));
        } else if (str_eq(cmd, "free")) {
            cmd_free();
        } else if (str_eq(cmd, "run")) {
            cmd_run(next_token(&pos, ' '));
        } else if (str_eq(cmd, "help")) {
            print_help();
        } else if (str_eq(cmd, "clear")) {
            cmd_clear();
        } else if (str_eq(cmd, "return")) {
            cmd_return(pos);   // the raw remainder after the "return" token
        } else {
            sys_print("unknown command: ");
            sys_print(cmd);
            sys_print("\n");
        }
    }
}
