// G.ELF: write 16384 bytes to fd 1 in a repeating, recognisable pattern, looping on
// partial writes, then exit 0. Nothing else.
//
// It is the UPSTREAM HALF OF A BACKPRESSURE TEST, and is kept this simple on purpose.
// `run g.elf | run count.elf` must print 16384, which is only possible if the writer
// BLOCKS on a full pipe and RESUMES rather than dropping bytes. A throwaway program
// proved this once and was deleted; G is the committed version, so the property has a
// standing test instead of a paragraph in an old report.
//
// THE SIZE MATTERS, and is not arbitrary. G_BYTES is four times the kernel's
// PIPE_SIZE (4096, kernel/pipe.h), so over one run the pipe fills and the writer must
// block and resume at least three times — that is the path under test. If PIPE_SIZE
// ever changes, keep G_BYTES a few times larger than it; a G_BYTES at or below
// PIPE_SIZE would let the whole write fit at once, and the test would pass without
// ever exercising backpressure.
//
// Run ALONE (`run g.elf`), it floods the screen with the pattern. That is expected,
// not a bug (see user/tests/README.md).

#include "../userlib.h"

#define G_BYTES 16384

// Filled with a 16-byte repeating pattern, so 16384 bytes is 1024 clean repeats and
// what reaches the screen (or a reader) is regular and recognisable rather than
// noise. Static (.bss), not on the stack: 16KB is large.
static char buf[G_BYTES];

void _start(void) {
    for (int i = 0; i < G_BYTES; i++) {
        buf[i] = "0123456789ABCDEF"[i & 15];
    }

    // Loop on partial writes. One sys_write moves at most the kernel staging buffer,
    // and a pipe takes only what fits, so a single call never moves all 16384; when
    // the pipe is full the call blocks and resumes once the reader drains it. A write
    // returning <= 0 means the reader downstream is gone, so stop.
    long done = 0;
    while (done < G_BYTES) {
        long w = sys_write(1, buf + done, (unsigned long)(G_BYTES - done));
        if (w <= 0) {
            break;
        }
        done += w;
    }

    sys_exit(0);
}
