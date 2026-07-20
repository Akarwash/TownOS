#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../drivers/disk.h"
#include "gdt.h"
#include "isr.h"
#include "timer.h"
#include "memory.h"
#include "heap.h"
#include "scheduler.h"

// The three ring-3 programs, linked into .user_text at 0x400000 (see
// user/user_program.c and linker.ld). Each loops forever printing its own
// letter; the scheduler switches between them on each timer tick.
extern void user_program_a(void);
extern void user_program_b(void);
extern void user_program_c(void);

// ==== TEMPORARY DISK SELF-TEST (remove once verified) =======================
// Single-block and multi-block write/read round-trip against the ATA driver.
// Prints DISK TEST: PASS or DISK TEST: FAIL at byte N. Delete this whole block
// and its two call sites in kernel_main once the driver is proven.
static void disk_selftest(void) {
    uint8_t buf[512];
    uint8_t buf2[512];

    // Single block at LBA 10: fill with a known pattern, write, zero, read back.
    for (int i = 0; i < 512; i++) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    if (disk_write(10, 1, buf) != 0) {
        print_string("DISK TEST: FAIL (write error)\n");
        return;
    }
    for (int i = 0; i < 512; i++) {
        buf2[i] = 0;
    }
    if (disk_read(10, 1, buf2) != 0) {
        print_string("DISK TEST: FAIL (read error)\n");
        return;
    }
    for (int i = 0; i < 512; i++) {
        if (buf[i] != buf2[i]) {
            print_string("DISK TEST: FAIL at byte ");
            print_int((uint32_t)i);
            print_string("\n");
            return;
        }
    }
    print_string("DISK TEST: PASS (single block)\n");

    // Multi-block: two blocks at LBA 20.
    uint8_t big[1024];
    uint8_t big2[1024];
    for (int i = 0; i < 1024; i++) {
        big[i] = (uint8_t)((i * 7) & 0xFF);
    }
    if (disk_write(20, 2, big) != 0) {
        print_string("DISK TEST: FAIL (multi write error)\n");
        return;
    }
    for (int i = 0; i < 1024; i++) {
        big2[i] = 0;
    }
    if (disk_read(20, 2, big2) != 0) {
        print_string("DISK TEST: FAIL (multi read error)\n");
        return;
    }
    for (int i = 0; i < 1024; i++) {
        if (big[i] != big2[i]) {
            print_string("DISK TEST: FAIL at byte ");
            print_int((uint32_t)i);
            print_string("\n");
            return;
        }
    }
    print_string("DISK TEST: PASS (multi block)\n");
}
// ==== END TEMPORARY DISK SELF-TEST ==========================================

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

    // ==== TEMPORARY DISK SELF-TEST (remove once verified) ====================
    // A disk read is invisible, so prove the driver round-trips bytes: write a
    // known pattern, read it back into a zeroed buffer, and compare. Runs while
    // the scheduler is still off, so nothing else touches the disk.
    disk_init();
    disk_selftest();
    // ==== END TEMPORARY DISK SELF-TEST =======================================

    print_string("Starting scheduler with three ring-3 tasks...\n");

    // Create three ring-3 tasks and hand off to the scheduler. Each task_create
    // now heap-allocates its task_t and asks the user-stack allocator for its own
    // stack slice (no more hardcoded stack tops). This is the LAST thing
    // kernel_main does: scheduler_start enters task 0 and never returns; from here
    // on the timer interrupt switches between the three tasks. If we ever reach
    // the loop below, the handoff silently failed.
    task_create((uint64_t)&user_program_a);
    task_create((uint64_t)&user_program_b);
    task_create((uint64_t)&user_program_c);
    scheduler_start();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
