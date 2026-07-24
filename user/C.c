// User program C: print "C" forever. Same shape as A.c. See user/A.c.

#include "userlib.h"

static volatile unsigned long iterations;

void _start(void) {
    for (;;) {
        sys_write("C");
        iterations++;
        user_delay();
    }
}
