#ifndef B1NIX_U_STRINGS_H
#define B1NIX_U_STRINGS_H

#include <string.h>

/* NB: the legacy BSD bcopy/bzero/bcmp are intentionally *not* declared here.
 * bash (and some other ports) ship their own definitions guarded by
 * !HAVE_BCOPY; providing ours in this header would collide with those. Ports
 * that reference them resolve to their own copies, and our build relaxes
 * implicit-function-declaration to a warning for that legacy code. */

/* Find first set bit (1-based); 0 if none. */
static inline int ffs(int i) {
	if (i == 0)
		return 0;
	int n = 1;
	unsigned int u = (unsigned int)i;
	while ((u & 1u) == 0u) {
		u >>= 1;
		n++;
	}
	return n;
}

#endif
