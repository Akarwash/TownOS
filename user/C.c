// User program C: print "C" forty times, then exit with status 3.
//
// The non-zero status is the point of this program. A, B and the shell all exit
// 0, which is also what an uninitialised field or a dropped value looks like; 3
// is a number nothing else in the system produces, so seeing `exited with status
// 3` at the prompt proves the value made the whole trip: from sys_exit's RDI,
// through the mask in task_exit, into the zombie's exit_status, out through
// task_wait's RAX, and back into a ring-3 program that prints it. See user/A.c
// for the rest of the shape.

#include "userlib.h"

static volatile unsigned long iterations;

#define C_ROUNDS  40

void _start(void) {
    for (unsigned long i = 0; i < C_ROUNDS; i++) {
        sys_write("C");
        iterations++;
        user_delay();
    }
    sys_exit(3);
}
