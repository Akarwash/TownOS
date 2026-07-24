#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../drivers/disk.h"
#include "../fs/fat32.h"
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
// TEMPORARY: filesystem self-test. Remove once verified.
// ===========================================================================
// A filesystem read is invisible: nothing on screen distinguishes correct file
// contents from plausible garbage. So check against contents known in advance,
// byte for byte. The files come from tools/mkdisk.sh, which is where these
// expected values are also written.
#define SELFTEST_HELLO_NAME    "HELLO.TXT"
#define SELFTEST_HELLO_TEXT    "Hello from FAT32!"
#define SELFTEST_BIG_NAME      "BIG.TXT"
#define SELFTEST_BIG_PATTERN   "0123456789ABCDEF"
#define SELFTEST_BIG_PATTERN_LEN 16
#define SELFTEST_BIG_SIZE      16384          // 1024 repetitions of the pattern
#define SELFTEST_MISSING_NAME  "NOPE.TXT"     // deliberately not on the disk

static void selftest_result(char *label, int passed) {
    print_string(passed ? "FAT32 TEST: PASS (" : "FAT32 TEST: FAIL (");
    print_string(label);
    print_string(")\n");
}

static void fat32_selftest(void) {
    if (fat32_init() != 0) {
        selftest_result("init", 0);
        return;
    }

    print_string("Root directory:\n");
    fat32_list_root();

    // Small file: one cluster, so this proves the directory lookup and the
    // cluster-to-block arithmetic, and nothing about chain following.
    char small[64];
    uint32_t small_size = 0;
    int ok = fat32_read_file(SELFTEST_HELLO_NAME, small, sizeof(small),
                             &small_size) == 0;
    if (ok) {
        print_string("HELLO.TXT contents: ");
        for (uint32_t i = 0; i < small_size; i++) {
            print_char(small[i]);
        }
        print_string("\n");
        char *expected = SELFTEST_HELLO_TEXT;
        for (uint32_t i = 0; i < small_size; i++) {
            if (expected[i] == '\0' || small[i] != expected[i]) {
                ok = 0;
                break;
            }
        }
        if (ok && expected[small_size] != '\0') {
            ok = 0;   // file is shorter than the string it should hold
        }
    }
    selftest_result("small file", ok);

    // Multi-cluster file: the important one. A single-cluster read can pass with
    // completely broken chain logic, so only this proves the chain is followed.
    uint8_t *big = (uint8_t *)kmalloc(SELFTEST_BIG_SIZE);
    if (big == NULL) {
        selftest_result("multi-cluster file (no memory)", 0);
    } else {
        uint32_t big_size = 0;
        int big_ok = fat32_read_file(SELFTEST_BIG_NAME, big, SELFTEST_BIG_SIZE,
                                     &big_size) == 0;
        if (big_ok && big_size != SELFTEST_BIG_SIZE) {
            big_ok = 0;
        }
        if (big_ok) {
            char *pattern = SELFTEST_BIG_PATTERN;
            for (uint32_t i = 0; i < big_size; i++) {
                if (big[i] != (uint8_t)pattern[i % SELFTEST_BIG_PATTERN_LEN]) {
                    big_ok = 0;
                    break;
                }
            }
        }
        print_string("BIG.TXT bytes read: ");
        print_int(big_size);
        print_string("\n");
        selftest_result("multi-cluster file", big_ok);
        kfree(big);
    }

    // A missing file must fail cleanly, not fault and not hang.
    char scratch[64];
    uint32_t scratch_size = 0;
    int missing_ok = fat32_read_file(SELFTEST_MISSING_NAME, scratch,
                                     sizeof(scratch), &scratch_size) == -1;
    selftest_result("missing file returns -1", missing_ok);
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

    fat32_selftest();   // TEMPORARY: remove once the filesystem is verified

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
