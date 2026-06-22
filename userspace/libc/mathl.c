/* Long-double math wrappers that delegate to the double libm (openlibm).
 *
 * Kept in their OWN translation unit (not stdlib.c) on purpose: they reference
 * `pow` etc. from libm, which the small freestanding userspace binaries do NOT
 * link. Isolating them here means only a consumer that actually calls powl (and
 * therefore links libm — e.g. V8's d8) pulls this object and its libm refs; the
 * tiny test binaries never do. */

extern double pow(double, double);

/* powl: b1nix has no 80-bit long-double pow; delegate to the double pow. The
 * extra precision is unused — V8 only needs it for duration-format scaling. */
long double powl(long double x, long double y) {
	return (long double)pow((double)x, (double)y);
}
