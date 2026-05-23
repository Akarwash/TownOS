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


