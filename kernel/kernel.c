#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "../shell/shell.h"

void kernel_main(void) {
    gdt_init();          // describe memory (flat segments)
    isr_install();       // install interrupt handlers, remap PIC, enable interrupts
    timer_init(100);     // 100 Hz heartbeat on IRQ0
    keyboard_init();     // listen for keypresses on IRQ1
    memory_init();       // physical frame allocator

    screen_clear();
    print_string("Welcome to MiniOS!\n");

    shell_init();

    // Idle: halt until the next interrupt (keypress or timer tick) wakes us.
    while (1) {
        __asm__ __volatile__("hlt");
    }
}
