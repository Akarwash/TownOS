#include "memory.h"
#include "../libc/mem.h"

// Manage a region of physical RAM as fixed-size frames, tracked by a bitmap
// (one bit per frame: 1 = used, 0 = free). The region starts safely above the
// kernel, which loads at 1 MB.
#define MEMORY_START  0x400000              // 4 MB
#define MAX_FRAMES    32768                 // covers 128 MB from MEMORY_START

// The ring-3 program's code (4-6M) and stack (6-8M) physically occupy the low
// end of the pool. See boot/boot.asm (PD[2]/PD[3]) and user/user_program.c.
// These frames must be reserved at init so the allocator never hands out memory
// that the running user program already lives on.
#define USER_REGION_START  0x400000         // 4 MB, ring-3 code (PD[2])
#define USER_REGION_END    0x800000         // 8 MB, top of ring-3 stack (PD[3])

// TODO: both the 8M identity map (boot/boot.asm) and the 128M pool size above
// are invented constants, not measured. Multiboot hands the kernel a memory map
// describing the machine's actual RAM, but kernel_main takes no arguments and
// so discards the Multiboot info pointer at boot. The real fix is to read that
// map and size both the identity map and this pool from it. Not done here.

static uint8_t frame_bitmap[MAX_FRAMES / 8];

static void set_bit(uint32_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static void clear_bit(uint32_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static int  test_bit(uint32_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

void memory_init(void) {
    memset(frame_bitmap, 0, sizeof(frame_bitmap));

    // Mark the ring-3 region (4-8M) as used. Those frames hold the live user
    // program and its stack, so handing them out would corrupt running code.
    // The first free frame therefore lands at 8M, which is above the identity
    // map: alloc_frame() returns it, but touching it faults until the map grows.
    uint32_t user_first = (USER_REGION_START - MEMORY_START) / FRAME_SIZE;
    uint32_t user_last  = (USER_REGION_END - MEMORY_START) / FRAME_SIZE;
    for (uint32_t i = user_first; i < user_last; i++) {
        set_bit(i);
    }
}

uint64_t alloc_frame(void) {
    uint32_t i;
    for (i = 0; i < MAX_FRAMES; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            return MEMORY_START + (uint64_t)i * FRAME_SIZE;
        }
    }
    return 0;   // out of memory
}

void free_frame(uint64_t addr) {
    if (addr < MEMORY_START) {
        return;
    }
    uint32_t frame = (addr - MEMORY_START) / FRAME_SIZE;
    if (frame < MAX_FRAMES) {
        clear_bit(frame);
    }
}

uint32_t frames_used(void) {
    uint32_t i, count = 0;
    for (i = 0; i < MAX_FRAMES; i++) {
        if (test_bit(i)) {
            count++;
        }
    }
    return count;
}
