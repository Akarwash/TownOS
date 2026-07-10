#include "memory.h"
#include "../libc/mem.h"

// Manage a region of physical RAM as fixed-size frames, tracked by a bitmap
// (one bit per frame: 1 = used, 0 = free). The region starts safely above the
// kernel, which loads at 1 MB.
#define MEMORY_START  0x400000              // 4 MB
#define MAX_FRAMES    32768                 // covers 128 MB from MEMORY_START

static uint8_t frame_bitmap[MAX_FRAMES / 8];

static void set_bit(uint32_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static void clear_bit(uint32_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static int  test_bit(uint32_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

void memory_init(void) {
    memset(frame_bitmap, 0, sizeof(frame_bitmap));
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
