#include "keyboard.h"
#include "ports.h"
#include "../kernel/isr.h"
#include "../shell/shell.h"
#include "../include/vectors.h"

#define KEYBOARD_DATA_PORT 0x60
#define KEY_RELEASE_MASK   0x80

// US QWERTY scan code (set 1) to ASCII. Index = scan code, 0 = unmapped/ignored.
// Key releases (bit 7 set) are handled before this table is consulted.
static const char scancode_to_ascii[128] = {
    0,    0,   '1',  '2',  '3',  '4',  '5',  '6',   // 0x00 - 0x07
    '7',  '8', '9',  '0',  '-',  '=',  '\b', '\t',  // 0x08 - 0x0F
    'q',  'w', 'e',  'r',  't',  'y',  'u',  'i',   // 0x10 - 0x17
    'o',  'p', '[',  ']',  '\n', 0,    'a',  's',   // 0x18 - 0x1F  (0x1C enter, 0x1D ctrl)
    'd',  'f', 'g',  'h',  'j',  'k',  'l',  ';',   // 0x20 - 0x27
    '\'', '`', 0,    '\\', 'z',  'x',  'c',  'v',   // 0x28 - 0x2F  (0x2A lshift)
    'b',  'n', 'm',  ',',  '.',  '/',  0,    '*',   // 0x30 - 0x37  (0x36 rshift)
    0,    ' ', 0,    0,    0,    0,    0,    0,      // 0x38 - 0x3F  (0x38 alt, 0x39 space)
    /* remaining entries default to 0 */
};

static void keyboard_callback(registers_t *regs) {
    (void)regs;
    uint8_t scancode = port_byte_in(KEYBOARD_DATA_PORT);

    // Ignore key-release events (high bit set).
    if (scancode & KEY_RELEASE_MASK) {
        return;
    }

    char c = scancode_to_ascii[scancode];
    if (c != 0) {
        shell_handle_keypress(c);
    }
}

void keyboard_init(void) {
    register_interrupt_handler(IRQ_KEYBOARD, keyboard_callback);
}
