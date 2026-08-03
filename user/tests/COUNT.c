// COUNT.ELF: read fd 0 until end of file, count the bytes, print the count to fd 1,
// exit 0.
//
// This is the downstream half of a pipe's "done" condition, and the reason it is a
// test fixture: piped after a writer (`run a.elf | run count.elf`) it blocks on the
// empty pipe until bytes arrive, and it only ever finishes because closing the last
// write end delivers EOF (a read returning 0). So a run that prints the right total
// and exits proves the block/wake path AND that a close is an event that unblocks an
// EOF-waiting reader (B2 in docs/decisions/0022).
//
// IT MUST LOOP ON PARTIAL READS. One sys_read moves at most the kernel's staging
// buffer, and a pipe hands over only what has been written so far, so a single read
// is never assumed to have drained the stream (B5). The loop ends only on 0 (EOF).

#include "../userlib.h"

// Print a non-negative number in decimal to fd 1. There is no libc, so the digits
// are built by hand, least significant first, into the end of a buffer. The do/while
// prints a single 0 for zero rather than nothing.
static void print_ulong(unsigned long v) {
    char buf[21];              // 20 digits is the most a 64-bit value needs, plus '\0'
    int i = 20;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    sys_print(&buf[i]);
}

void _start(void) {
    char buf[256];
    unsigned long total = 0;

    for (;;) {
        long n = sys_read(0, buf, sizeof(buf));
        if (n <= 0) {
            break;             // 0 = EOF (last writer closed); < 0 = read error
        }
        total += (unsigned long)n;
    }

    print_ulong(total);
    sys_print("\n");
    sys_exit(0);
}
