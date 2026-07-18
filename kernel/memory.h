#ifndef MEMORY_H
#define MEMORY_H

#include "../include/types.h"

#define FRAME_SIZE 4096

// The ring-3 program's code (4-6M) and stack (6-8M) physically occupy the low
// end of the frame pool. See boot/boot.asm (PD[2]/PD[3]) and
// user/user_program.c. memory_init() reserves these frames so the allocator
// never hands out memory the running user program already lives on. They are
// exposed here (not kept file-local to memory.c) so the syscall layer can reuse
// them to bound-check untrusted pointers from ring 3.
#define USER_REGION_START  0x400000         // 4 MB, ring-3 code (PD[2])
#define USER_REGION_END    0x800000         // 8 MB, top of ring-3 stack (PD[3])

void memory_init(void);
uint64_t alloc_frame(void);        // returns a free physical frame address, or 0 if none
void free_frame(uint64_t addr);
uint32_t frames_used(void);

#endif
