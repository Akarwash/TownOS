// User program B: print "B" forever. Same shape as A.c; a separate binary so
// the scheduler has three distinct files to interleave and the loader is proven
// on more than one. See user/A.c for the details.

#include "userlib.h"

static volatile unsigned long iterations;

void _start(void) {
    for (;;) {
        sys_write("B");
        iterations++;
        user_delay();
    }
}
