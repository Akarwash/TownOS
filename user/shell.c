// The MiniOS interactive shell, as a ring-3 program.
//
// This is the capstone of the syscall boundary. The shell is a fully fenced-in
// user program: it is loaded off the disk like any other (as SHELL.ELF), runs at
// CPL 3 in its own address space, and cannot touch the keyboard, the screen, the
// filesystem, or the loader except through `int 0x50`. Everything it does, reading
// a key, echoing it, listing files, printing a file, launching a program, it does
// with nothing but the syscalls in userlib.h. That it works at all is the proof
// that the boundary is complete.
//
// Built and linked exactly like user/A.c (see the SHELL.ELF rule in the Makefile
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

    sys_write(p);
}

static void print_help(void) {
    // The command names are MiniOS's own and deliberately not the Unix ones.
    sys_write("commands:\n");
    sys_write("  list           list files in the root directory\n");
    sys_write("  read <file>    print a file's contents\n");
    sys_write("  run <file>     run a program and wait for it to finish\n");
    sys_write("  help           show this list\n");
    sys_write("  clear          clear the screen\n");
    sys_write("  return <text>  print the text back\n");
}

static void cmd_list(void) {
    if (sys_list(list_buf, sizeof(list_buf)) == SYS_FAIL) {
        sys_write("list: could not read the directory\n");
        return;
    }
    // The kernel filled list_buf with newline-separated names, NUL-terminated.
    sys_write(list_buf);
}

static void cmd_read(char *name) {
    if (name == (char *)0) {
        sys_write("read: missing filename\n");
        return;
    }
    // Ask for at most SHELL_FILE_MAX - 1 bytes so there is room to terminate.
    unsigned long n = sys_readfile(name, file_buf, sizeof(file_buf) - 1);
    if (n == SYS_FAIL) {
        sys_write("read: cannot read ");
        sys_write(name);
        sys_write("\n");
        return;
    }
    // File contents are raw and not NUL-terminated, so terminate before printing.
    file_buf[n] = '\0';
    sys_write(file_buf);
    sys_write("\n");   // the file may not end in a newline; keep the prompt tidy
}

static void cmd_run(char *name) {
    if (name == (char *)0) {
        sys_write("run: missing filename\n");
        return;
    }
    if (sys_run(name) == SYS_FAIL) {
        sys_write("run: could not start ");
        sys_write(name);
        sys_write("\n");
        return;
    }
    // The program is now a task of its own and its output interleaves with this
    // shell from the next timer tick on. Announce it before waiting, so the letters
    // that follow are visibly attributed to something that was started.
    sys_write("run: started ");
    sys_write(name);
    sys_write("\n");

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
        sys_write("run: no child to wait for\n");
        return;
    }

    sys_write("run: ");
    sys_write(name);
    sys_write(" exited with status ");
    print_uint((unsigned long)status);
    sys_write("\n");
}

static void cmd_clear(void) {
    for (int i = 0; i < SHELL_CLEAR_LINES; i++) {
        sys_write("\n");
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
    sys_write(rest);
    sys_write("\n");
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
            sys_write("\n");   // echo the newline that ends the line
            break;
        }

        if (c == '\b') {
            // Backspace: drop the last character if there is one, and erase it on
            // screen (the screen driver's '\b' rubs out the glyph). Guarding on
            // len > 0 means backspace cannot chew back into the prompt.
            if (len > 0) {
                len--;
                sys_write("\b");
            }
            continue;
        }

        // A printable character. Append it only if the fixed buffer has room (one
        // slot reserved for '\0'); otherwise DROP it rather than overflow. Echo it
        // so the user sees what they type.
        if (len < SHELL_LINE_MAX - 1) {
            line[len++] = c;
            char echo[2] = { c, '\0' };
            sys_write(echo);
        }
    }

    line[len] = '\0';
}

// The entry point named by user.ld's ENTRY(_start). The loader takes the entry
// address from the ELF header, so this symbol only has to match the linker script.
void _start(void) {
    sys_write("MiniOS shell. type 'help'.\n");

    for (;;) {
        sys_write("> ");
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
        } else if (str_eq(cmd, "run")) {
            cmd_run(next_token(&pos, ' '));
        } else if (str_eq(cmd, "help")) {
            print_help();
        } else if (str_eq(cmd, "clear")) {
            cmd_clear();
        } else if (str_eq(cmd, "return")) {
            cmd_return(pos);   // the raw remainder after the "return" token
        } else {
            sys_write("unknown command: ");
            sys_write(cmd);
            sys_write("\n");
        }
    }
}
