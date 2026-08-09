#ifndef QUODLIBET_COMPAT_SYS_TIME_H
#define QUODLIBET_COMPAT_SYS_TIME_H

#include <time.h>

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* Process time, not wall-clock time. The pinned sources use this only for
   reported statistics, never for a decision, so the difference cannot reach a
   verdict. */
static int gettimeofday(struct timeval *tv, void *zone) {
    const clock_t ticks = clock();
    (void)zone;
    tv->tv_sec = (long)(ticks / CLOCKS_PER_SEC);
    tv->tv_usec = (long)((ticks % CLOCKS_PER_SEC) * (1000000 / CLOCKS_PER_SEC));
    return 0;
}

#endif
