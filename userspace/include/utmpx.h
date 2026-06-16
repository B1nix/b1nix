#ifndef B1NIX_U_UTMPX_H
#define B1NIX_U_UTMPX_H

#include <utmp.h>

#define utmpx utmp

#define getutxent getutent
#define getutxid getutid
#define getutxline getutline
#define pututxline pututline
#define setutxent setutent
#define endutxent endutent
#define utmpxname utmpname
#define updwtmpx updwtmp

#endif
