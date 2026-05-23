#ifndef MEM_H
#define MEM_H

#include "../include/types.h"

void memcpy(void *dest, const void *src, size_t n);
void memset(void *dest, uint8_t val, size_t n);

#endif