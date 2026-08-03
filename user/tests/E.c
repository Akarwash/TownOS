// User program E: print "E" fifteen times, then exit with status 7.
//
// NOBODY WILL EVER READ THAT 7, and that is the point. E is started by D
// (user/tests/D.c), which exits without waiting, so by the time E finishes its
// parent's slot in the task table is NULL. An exit status with no reader is not
// kept: the scheduler's sweeper frees E's tombstone along with its address space
// rather than preserving a fact nobody can ask for. E exists to be orphaned.
//
// Seven is arbitrary and chosen only to be distinctive. If it ever does appear at
// the prompt, something collected a status that should have been dropped, and the
// odd number is what makes that obvious rather than plausible.
//
// Fifteen rounds is short enough to finish while somebody is still watching the
// prompt and long enough to outlive D by a visible margin, so the E's printing
// beside a live prompt are the proof that a parent exiting does not take its
// children with it. Running E directly (`run e.elf`) is uninteresting: the shell
// waits for it like any other program and status 7 is duly printed.

#include "../userlib.h"

static volatile unsigned long iterations;

#define E_ROUNDS  15

void _start(void) {
    for (unsigned long i = 0; i < E_ROUNDS; i++) {
        sys_print("E");
        iterations++;
        user_delay();
    }
    sys_exit(7);
}
