#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "heap.h"
#include "scheduler.h"

// The two ring-3 programs, linked into .user_text at 0x400000 (see
// user/user_program.c and linker.ld). Each loops forever printing its own
// letter; the scheduler switches between them on each timer tick.
extern void user_program_a(void);
extern void user_program_b(void);

void kernel_main(uint64_t multiboot_info_addr) {
    gdt_init();          // describe memory (flat segments) + load the TSS
    isr_install();       // install interrupt handlers, remap PIC, enable interrupts
    timer_init(100);     // 100 Hz heartbeat on IRQ0
    keyboard_init();     // listen for keypresses on IRQ1

    screen_clear();
    print_string("Welcome to MiniOS!\n");

    // Read the real amount of RAM from the Multiboot map, extend the identity map
    // to cover it (capped at 1GB), and flush the TLB. This must happen before
    // memory_init so every frame the allocator manages is actually mapped.
    uint64_t top_of_ram = memory_detect_and_map(multiboot_info_addr);
    print_string("Detected RAM: ");
    print_int((uint32_t)(top_of_ram >> 20));   // 0 means the no-map fallback ran
    print_string(" MB\n");
    memory_init(multiboot_info_addr);          // size the frame pool from real RAM

    heap_init();        // build the kernel heap on top of the frame allocator

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
