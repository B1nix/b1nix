// Helpers a few openlibm members reference (pulled in when openlibm is
// --whole-archive'd into the shared libc.so.1) but that neither openlibm nor the
// rest of the b1nix libc define: the complex real/imag accessors and the
// long-double scalbn. Small, self-contained, no external math dependencies.

// openlibm already provides creal/cimag/creall/cimagl; only the float accessors
// and the long-double scalbn are missing.
float crealf(float _Complex z) { return __real__ z; }
float cimagf(float _Complex z) { return __imag__ z; }

// scalbnl(x, n) = x * 2^n. A plain scale loop — correct for the finite n the
// math routines use; no powl/ldexpl dependency.
long double scalbnl(long double x, int n) {
	long double r = x;
	while (n > 0) { r *= 2.0L; n--; }
	while (n < 0) { r *= 0.5L; n++; }
	return r;
}
