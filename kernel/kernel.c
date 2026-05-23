#include "../drivers/screen.h"

void kernel_main(void) {
    screen_clear();
    print_string("Hello from MiniOS!");
}