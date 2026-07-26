#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);

// Pop one character from the keyboard ring buffer, or return 0 if none is waiting.
// The consumer end of the producer/consumer pair driven by the keyboard IRQ; the
// SYS_READKEY handler (kernel/syscall.c) is its only caller. See the ring buffer
// in drivers/keyboard.c for the empty sentinel and the full-drop policy.
int keyboard_getchar(void);

#endif
