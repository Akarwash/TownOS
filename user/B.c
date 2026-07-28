// User program B: print "B" sixty times, then exit with status 0. Same shape as
// A.c; a separate binary so the scheduler has three distinct files to interleave
// and the loader is proven on more than one. It runs three times as long as A so
// the two are visibly different lengths of work. See user/A.c for the details,
// including why the loop must be bounded.

#include "userlib.h"

static volatile unsigned long iterations;

#define B_ROUNDS  60

void _start(void) {
    for (unsigned long i = 0; i < B_ROUNDS; i++) {
        sys_write("B");
        iterations++;
        user_delay();
    }
    sys_exit(0);
}
