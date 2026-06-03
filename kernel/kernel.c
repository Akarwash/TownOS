#include "../drivers/screen.h"
#include "gdt.h"

void kernel_main(void) {
    gdt_init();
    screen_clear();
    print_string("Hello from MiniOS!");
}