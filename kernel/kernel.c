#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../drivers/disk.h"
#include "../fs/fat32.h"
#include "elf.h"
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

// ===========================================================================
// TEMPORARY: ELF manifest test. Remove once the loader itself works.
// ===========================================================================
// Reads one program off the disk and prints the manifest the loader will act on,
// without loading anything. Parsing is proven in isolation this way, with
// nothing at risk: no frames allocated, no pages mapped, nothing written to any
// address the file names.
#define ELF_TEST_PROGRAM "A.ELF"

static void elf_manifest_test(void) {
    uint32_t size = 0;
    if (fat32_stat(ELF_TEST_PROGRAM, &size) != 0) {
        print_string("elf test: cannot stat " ELF_TEST_PROGRAM "\n");
        return;
    }

    void *buf = kmalloc(size);
    if (buf == NULL) {
        print_string("elf test: out of memory for " ELF_TEST_PROGRAM "\n");
        return;
    }

    uint32_t read_size = 0;
    if (fat32_read_file(ELF_TEST_PROGRAM, buf, size, &read_size) != 0) {
        print_string("elf test: cannot read " ELF_TEST_PROGRAM "\n");
        kfree(buf);
        return;
    }

    print_string("elf test: " ELF_TEST_PROGRAM " is ");
    print_int(read_size);
    print_string(" bytes\n");
    elf_print_manifest(buf, read_size, ELF_TEST_PROGRAM);

    kfree(buf);
}

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

    disk_init();        // probe the primary ATA bus and silence its IRQ line

    if (fat32_init() != 0) {
        print_string("FAT32: mount failed, programs cannot be loaded\n");
    }

    elf_manifest_test();   // TEMPORARY: remove once the loader works

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
