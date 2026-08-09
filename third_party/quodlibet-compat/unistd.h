/* The POSIX surface the pinned SAT sources use, mapped onto the Win32 CRT.
   Only what those sources actually call is provided; a gap should be a
   compile error rather than a silently wrong stub. */
#ifndef QUODLIBET_COMPAT_UNISTD_H
#define QUODLIBET_COMPAT_UNISTD_H

#include <io.h>
#include <process.h>
#include <stdio.h>

#define R_OK 4
#define W_OK 2
#define X_OK 0
#define F_OK 0
#define _SC_PAGESIZE 1

#define isatty _isatty
#define access _access
#define popen _popen
#define pclose _pclose

/* Reported only in resource statistics that no verdict depends on. */
static long sysconf(int name) {
    (void)name;
    return 4096L;
}

#endif
