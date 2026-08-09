#ifndef QUODLIBET_COMPAT_SYS_RESOURCE_H
#define QUODLIBET_COMPAT_SYS_RESOURCE_H

#include <sys/time.h>
#include <time.h>

#define RUSAGE_SELF 0

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
};

/* Statistics only, as with gettimeofday above. */
static int getrusage(int who, struct rusage *usage) {
    const clock_t ticks = clock();
    (void)who;
    usage->ru_utime.tv_sec = (long)(ticks / CLOCKS_PER_SEC);
    usage->ru_utime.tv_usec =
        (long)((ticks % CLOCKS_PER_SEC) * (1000000 / CLOCKS_PER_SEC));
    usage->ru_stime.tv_sec = 0;
    usage->ru_stime.tv_usec = 0;
    usage->ru_maxrss = 0;
    return 0;
}

#endif
