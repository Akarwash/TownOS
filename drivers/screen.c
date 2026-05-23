#include "screen.h"
#include "ports.h"
#include "../libc/mem.h"

static int get_cursor(void) {
    port_byte_out(0x3D4, 14);
    int offset = port_byte_in(0x3D5) << 8;
    port_byte_out(0x3D4, 15);
    offset += port_byte_in(0x3D5);
    return offset;
}

static void set_cursor(int offset) {
    port_byte_out(0x3D4, 14);
    port_byte_out(0x3D5, (uint8_t)(offset >> 8));
    port_byte_out(0x3D4, 15);
    port_byte_out(0x3D5, (uint8_t)(offset & 0xFF));
}

static void scroll(void) {
    char *video = (char *)VIDEO_ADDRESS;
    int i;

    memcpy(video, video + MAX_COLS * 2, MAX_COLS * (MAX_ROWS - 1) * 2);

    char *last_row = video + (MAX_ROWS - 1) * MAX_COLS * 2;
    for (i = 0; i < MAX_COLS; i++) {
        last_row[i * 2] = ' ';
        last_row[i * 2 + 1] = WHITE_ON_BLACK;
    }
}

void screen_clear(void) {
    char *video = (char *)VIDEO_ADDRESS;
    int i;
    for (i = 0; i < MAX_ROWS * MAX_COLS; i++) {
        video[i * 2] = ' ';
        video[i * 2 + 1] = WHITE_ON_BLACK;
    }
    set_cursor(0);
}

void print_char(char c) {
    char *video = (char *)VIDEO_ADDRESS;
    int offset = get_cursor();

    if (c == '\n') {
        int row = offset / MAX_COLS;
        offset = (row + 1) * MAX_COLS;
    } 
    else {
        video[offset * 2] = c;
        video[offset * 2 + 1] = WHITE_ON_BLACK;
        offset++;
    }

    if (offset >= MAX_ROWS * MAX_COLS) {
        scroll();
        offset = (MAX_ROWS - 1) * MAX_COLS;
    }

    set_cursor(offset);
}


void print_string(char *str) {
    int i = 0;
    while (str[i] != '\0') {
        print_char(str[i]);
        i++;
    }
}

