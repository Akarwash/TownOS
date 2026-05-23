#include "mem.h"

void memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void memset(void *dest, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = val;
    }
}