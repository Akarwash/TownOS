// User program D: start E.ELF and exit immediately, WITHOUT waiting for it.
//
// D EXISTS TO ORPHAN E, and the missing sys_wait below is the whole program. Do
// not "fix" it by adding one: a D that waited would make E an ordinary child
// reaped by an ordinary parent, and this fixture would test nothing at all.
//
// What it is for. Two pieces of kernel code are unreachable in every other test:
// the free path inside reap_sweep, and the branch of parent_alive that answers "no"
// because the parent's slot is NULL. Both are unreachable for the same reason. When
// a child exits, task_exit wakes its parent and switches straight to it, the
// parent's iretq lands on the `int 0x50` its block rewound onto, and it re-enters
// SYS_WAIT and reaps the child before a single timer tick has gone by. The sweeper
// never gets a look. The only way to reach it is a zombie whose parent will never
// call wait, and the only way to guarantee that is a parent that is already dead.
//
// So: the shell runs D and waits for it. D starts E and exits. The shell reaps D
// and goes back to its prompt. E is still running with a parent_id pointing at a
// slot that is now NULL, and when E exits nobody can collect it, so the sweeper
// does — which is the first time that code has ever run. See user/tests/README.md.

#include "../userlib.h"

void _start(void) {
    sys_print("D: starting E\n");

    // Not checked, deliberately: if E.ELF is missing the kernel says so and this
    // program has nothing useful to add. D's job is to leave, not to supervise.
    sys_run("E.ELF", -1, -1);   // fresh console for E; D orphans it and does not wait

    sys_print("D: not waiting, exiting\n");

    // NO sys_wait() HERE. See the comment at the top of this file before changing
    // anything about that.
    sys_exit(0);
}
