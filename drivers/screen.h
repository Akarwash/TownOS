#ifndef SCREEN_H
#define SCREEN_H

#include "../include/types.h"

#define VIDEO_ADDRESS   0xB8000
#define MAX_ROWS        25
#define MAX_COLS        80
#define WHITE_ON_BLACK  0x0F

// public functions
void screen_clear(void);
void print_char(char c);
void print_string(char *str);
void print_int(uint32_t n);
void print_hex(uint64_t n);   // "0x..." form, for addresses, sizes and offsets

#endif