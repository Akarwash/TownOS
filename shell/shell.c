#include "shell.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include "../kernel/timer.h"

#define INPUT_MAX 256

static char input_buffer[INPUT_MAX];
static int buffer_pos;

static void print_prompt(void) {
    print_string("> ");
}

static void shell_execute(char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        print_string("Available commands:\n");
        print_string("  help   - show this list\n");
        print_string("  clear  - clear the screen\n");
        print_string("  hello  - print a greeting\n");
        print_string("  tick   - show timer tick count\n");
    } else if (strcmp(cmd, "clear") == 0) {
        screen_clear();
    } else if (strcmp(cmd, "hello") == 0) {
        print_string("Hello from MiniOS!\n");
    } else if (strcmp(cmd, "tick") == 0) {
        print_int(get_tick());
        print_char('\n');
    } else if (cmd[0] != '\0') {
        print_string("Unknown command: ");
        print_string(cmd);
        print_char('\n');
    }
}

void shell_init(void) {
    buffer_pos = 0;
    input_buffer[0] = '\0';
    print_prompt();
}

void shell_handle_keypress(char c) {
    if (c == '\n') {
        print_char('\n');
        input_buffer[buffer_pos] = '\0';
        shell_execute(input_buffer);
        buffer_pos = 0;
        input_buffer[0] = '\0';
        print_prompt();
    } else if (c == '\b') {
        if (buffer_pos > 0) {
            buffer_pos--;
            print_char('\b');
        }
    } else if (buffer_pos < INPUT_MAX - 1) {
        input_buffer[buffer_pos++] = c;
        print_char(c);
    }
}
