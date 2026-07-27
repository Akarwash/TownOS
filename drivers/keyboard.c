#include "keyboard.h"
#include "ports.h"
#include "../kernel/isr.h"
#include "../kernel/scheduler.h"
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

// ---------------------------------------------------------------------------
// The keyboard ring buffer: a fixed circular queue between the keyboard IRQ
// (the producer) and SYS_READKEY (the consumer, in kernel/syscall.c).
// ---------------------------------------------------------------------------
// The IRQ runs with interrupts held off and must be short, so it does the least
// possible work: decode one scancode and drop the character here. The consumer, a
// ring-3 program calling through the syscall gate, drains the buffer at its own
// pace. Producer and consumer never run at the same instant (single CPU, and both
// the keyboard IRQ gate and the syscall gate clear IF on entry), so the two
// indices need no lock.
//
// THE TWO INDICES. write_index is the slot the next produced character goes into;
// read_index is the slot the next consumed character comes out of. They chase each
// other around a ring of KBD_BUFFER_SIZE slots, each advance stepping forward one
// slot and wrapping modulo the size.
//   write_index == read_index  means EMPTY (the consumer has caught up).
// To keep EMPTY distinguishable from FULL with that single rule, one slot is always
// left unused: the ring holds at most KBD_BUFFER_SIZE - 1 characters at once. If a
// full buffer were allowed to wrap write_index onto read_index, "full" would look
// exactly like "empty" and the buffer would appear to silently discard everything.
#define KBD_BUFFER_SIZE 128

static char     kbd_buffer[KBD_BUFFER_SIZE];
static uint32_t kbd_write_index;   // producer's cursor (next slot to fill)
static uint32_t kbd_read_index;    // consumer's cursor (next slot to drain)

// Producer helper, called only from the keyboard IRQ. Push one character unless
// the ring is full.
//
// FULL POLICY: if advancing write_index would land on read_index, the ring is full
// and the new character is DROPPED, not stored. Dropping is the deliberate choice.
// The alternative, overwriting read_index and stepping past it, would destroy input
// the consumer has already been promised and would also collapse the empty/full
// distinction above. A dropped keypress can simply be retyped; a silently mangled
// queue cannot be recovered.
static void kbd_buffer_push(char c) {
    uint32_t next = (kbd_write_index + 1) % KBD_BUFFER_SIZE;
    if (next == kbd_read_index) {
        return;   // full: drop the new key, see FULL POLICY above
    }
    kbd_buffer[kbd_write_index] = c;
    kbd_write_index = next;
}

// Consumer side, called from the SYS_READKEY handler (kernel/syscall.c). Pop one
// character, or return 0 when the ring is empty.
//
// 0 is a safe "nothing waiting" sentinel: the scancode table maps every unmapped
// key to 0 and keyboard_callback only pushes non-zero characters, so a real 0 never
// enters the ring and cannot be mistaken for "empty".
int keyboard_getchar(void) {
    if (kbd_read_index == kbd_write_index) {
        return 0;   // empty: the consumer has caught up to the producer
    }
    char c = kbd_buffer[kbd_read_index];
    kbd_read_index = (kbd_read_index + 1) % KBD_BUFFER_SIZE;
    return (int)(unsigned char)c;
}

// The producer. An interrupt handler runs with interrupts held off (the gate is an
// interrupt gate, so the CPU cleared IF on entry), so it must be short: decode one
// scancode, drop the character into the ring buffer, and return. It must NOT echo
// and must NOT run any command. A ring-3 program drains the buffer through
// SYS_READKEY and decides what to do with each key. (Before the shell became a
// ring-3 program this called shell_handle_keypress and did real work inside the
// IRQ, which is exactly what the ring buffer now keeps out of interrupt context.)
static void keyboard_callback(registers_t *regs) {
    (void)regs;
    uint8_t scancode = port_byte_in(KEYBOARD_DATA_PORT);

    // Ignore key-release events (high bit set).
    if (scancode & KEY_RELEASE_MASK) {
        return;
    }

    char c = scancode_to_ascii[scancode];
    if (c != 0) {
        kbd_buffer_push(c);

        // This IRQ is what CAUSES the event tasks blocked on WAIT_KEY are waiting
        // for, so waking them is its job: a sleeping task cannot notice a key
        // arriving, because it is not running. Waking only marks them READY, it does
        // not switch to them, which keeps this handler short and leaves the choice
        // of what runs next where it belongs, in the scheduler on the next tick.
        //
        // Ordering matters: push first, wake second. A task woken before the
        // character was in the buffer could be scheduled, re-issue its read, find
        // nothing, and block again, turning one keypress into a wasted round trip.
        scheduler_wake(WAIT_KEY);
    }
}

void keyboard_init(void) {
    register_interrupt_handler(IRQ_KEYBOARD, keyboard_callback);
}
