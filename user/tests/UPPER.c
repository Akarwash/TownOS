// UPPER.ELF: read fd 0 until end of file, uppercase each byte, write it to fd 1,
// exit 0.
//
// A STREAMING MIDDLE STAGE: it has a pipe on BOTH sides, so
// `run a.elf | run upper.elf | run count.elf` exercises three stages and a program
// that both reads and writes a pipe. It passes bytes through as it reads them rather
// than buffering the whole stream, so it also shows a pipeline making progress
// incrementally.
//
// IT MUST LOOP ON PARTIAL READS AND PARTIAL WRITES (B5). A read hands over only what
// is in the pipe; a write into a pipe takes only what fits and returns that count,
// so the remainder must be retried — and when the pipe ahead of it fills, the write
// blocks until the next stage drains it (backpressure), which is correct, not a
// hang. A write that returns <= 0 means the stage downstream has gone, so it stops.

#include "../userlib.h"

void _start(void) {
    char buf[256];

    for (;;) {
        long n = sys_read(0, buf, sizeof(buf));
        if (n <= 0) {
            break;             // 0 = EOF; < 0 = read error
        }

        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c >= 'a' && c <= 'z') {
                buf[i] = (char)(c - 'a' + 'A');
            }
        }

        // Write all n bytes, looping over partial writes. A pipe takes only what
        // fits, so one sys_write is never assumed to have moved the whole chunk.
        long done = 0;
        while (done < n) {
            long w = sys_write(1, buf + done, (unsigned long)(n - done));
            if (w <= 0) {
                sys_exit(0);   // the reader ahead of us is gone: nothing left to do
            }
            done += w;
        }
    }

    sys_exit(0);
}
