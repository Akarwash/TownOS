#ifndef MEMORY_H
#define MEMORY_H

#include "../include/types.h"

#define FRAME_SIZE 4096

void memory_init(void);
uint32_t alloc_frame(void);        // returns a free physical frame address, or 0 if none
void free_frame(uint32_t addr);
uint32_t frames_used(void);

#endif
