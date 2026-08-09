/* Force-included ahead of the pinned SAT sources on Windows.

   These names exist on POSIX inside headers the platform does have, so they
   cannot be supplied by adding a header of their own without shadowing the
   CRT's. A prelude adds them after the real header is read. */
#ifndef QUODLIBET_COMPAT_SAT_PRELUDE_H
#define QUODLIBET_COMPAT_SAT_PRELUDE_H

#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

/* Signal numbers the CRT does not define. A handler registered for one of
   these simply never fires on Windows, which is the intended behaviour: the
   solver runs under Quodlibet's own deadline and output caps. */
#ifndef SIGBUS
#define SIGBUS 10
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) (((mode) & _S_IFMT) == _S_IFIFO)
#endif

/* CaDiCaL selects these through NUNLOCKED, but lrat-check.c calls them
   directly. The locked forms are correct, only slower. */
#ifndef getc_unlocked
#define getc_unlocked getc
#endif
#ifndef putc_unlocked
#define putc_unlocked putc
#endif

#endif
