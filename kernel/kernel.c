#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "usermode.h"

// The first ring-3 program, linked into .user_text at 0x400000 (see
// user/user_program.c and linker.ld).
extern void user_program(void);

void kernel_main(void) {
    gdt_init();          // describe memory (flat segments) + load the TSS
    isr_install();       // install interrupt handlers, remap PIC, enable interrupts
    timer_init(100);     // 100 Hz heartbeat on IRQ0
    keyboard_init();     // listen for keypresses on IRQ1
    memory_init();       // physical frame allocator

    screen_clear();
    print_string("Welcome to MiniOS!\n");
    print_string("Dropping to ring 3 (CPL 3)...\n");

    // Hand off to the ring-3 program. This is the LAST thing kernel_main does:
    // enter_user_mode never returns. The program runs at CPL 3 in its own pages,
    // executes a privileged instruction, and faults back into the kernel, which
    // reports the fault and halts. If we ever reach the loop below, the drop to
    // ring 3 silently failed.
    enter_user_mode((uint64_t)&user_program, USER_STACK_TOP);

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
