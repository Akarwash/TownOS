#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "scheduler.h"

// The two ring-3 programs, linked into .user_text at 0x400000 (see
// user/user_program.c and linker.ld). Each loops forever printing its own
// letter; the scheduler switches between them on each timer tick.
extern void user_program_a(void);
extern void user_program_b(void);

void kernel_main(void) {
    gdt_init();          // describe memory (flat segments) + load the TSS
    isr_install();       // install interrupt handlers, remap PIC, enable interrupts
    timer_init(100);     // 100 Hz heartbeat on IRQ0
    keyboard_init();     // listen for keypresses on IRQ1
    memory_init();       // physical frame allocator

    screen_clear();
    print_string("Welcome to MiniOS!\n");
    print_string("Starting scheduler with two ring-3 tasks...\n");

    // Create the two ring-3 tasks (each gets its own half of the 6-8M stack page)
    // and hand off to the scheduler. This is the LAST thing kernel_main does:
    // scheduler_start enters task 0 and never returns; from here on the timer
    // interrupt switches between the two tasks. If we ever reach the loop below,
    // the handoff silently failed.
    task_create((uint64_t)&user_program_a, TASK0_STACK_TOP);
    task_create((uint64_t)&user_program_b, TASK1_STACK_TOP);
    scheduler_start();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
