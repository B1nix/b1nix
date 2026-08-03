/* M51 rung 0: verify the ported libm (openlibm) actually computes at runtime.
 * Inputs are volatile so the compiler cannot constant-fold the calls away —
 * this is exactly the path the old recursive-inline math.h turned into a
 * `jmp .` hang. */
#include <math.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }

static int approx(double a, double b) {
  double d = a - b;
  if (d < 0)
    d = -d;
  return d < 1e-9;
}

int main(void) {
  volatile double half = 0.5, two = 2.0, ten = 10.0, one = 1.0, e = M_E;
  volatile double seven = 7.0, three = 3.0, x = 2.7, y = 2.1;

  if (!approx(sin(half), 0.479425538604203) ||
      !approx(cos(half), 0.877582561890373) ||
      !approx(tan(half), 0.546302489843790) ||
      !approx(pow(two, ten), 1024.0) ||
      !approx(exp(one), 2.718281828459045) ||
      !approx(log(e), 1.0) ||
      !approx(log10(pow(ten, three)), 3.0) ||
      !approx(atan2(one, one), M_PI_4) ||
      !approx(sqrt(two), 1.414213562373095) ||
      !approx(hypot(three, two * two), 5.0) ||
      !approx(fmod(seven, three), 1.0) ||
      !approx(floor(x), 2.0) || !approx(ceil(y), 3.0)) {
    mark("M51-GFX: fail libm\n");
    return 1;
  }
  mark("M51-GFX: ok libm\n");
  return 0;
}
