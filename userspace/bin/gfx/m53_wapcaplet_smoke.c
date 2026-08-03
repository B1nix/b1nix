/* M53 libwapcaplet smoke: prove the ported NetSurf string-internment library
 * works on b1nix. libwapcaplet is the lowest layer of the NetSurf browser lib
 * chain (libcss/libdom intern every selector and attribute name through it).
 *
 * Every marker is gated on real behaviour, nothing faked:
 *  - interning the same bytes twice returns the SAME pointer (the whole point of
 *    internment — O(1) pointer-equality comparison),
 *  - interning different bytes returns a DIFFERENT pointer,
 *  - case-insensitive comparison matches "Hello"/"hELLO" but the pointers differ,
 *  - lwc_string_data/length expose the original bytes intact.
 * Markers (M53-WAPCAPLET: ...) consumed by smoke.sh. */

#include <string.h>
#include <unistd.h>
#include <libwapcaplet/libwapcaplet.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

int main(void) {
  emit("M53-WAPCAPLET: start\n");

  lwc_string *a = NULL, *a2 = NULL, *b = NULL, *upper = NULL;

  /* ── Interning: same bytes twice must yield the identical pointer ── */
  if (lwc_intern_string("display", 7, &a) != lwc_error_ok || a == NULL) {
    emit("M53-WAPCAPLET: fail intern\n");
    return 1;
  }
  if (lwc_intern_string("display", 7, &a2) != lwc_error_ok || a2 == NULL) {
    emit("M53-WAPCAPLET: fail intern\n");
    return 1;
  }
  bool eq = false;
  lwc_string_isequal(a, a2, &eq);
  if (!eq || a != a2) {
    emit("M53-WAPCAPLET: fail intern-identity\n");
    return 1;
  }
  emit("M53-WAPCAPLET: ok intern\n");

  /* ── A genuinely different string must intern to a different pointer ── */
  if (lwc_intern_string("color", 5, &b) != lwc_error_ok || b == NULL) {
    emit("M53-WAPCAPLET: fail intern\n");
    return 1;
  }
  eq = true;
  lwc_string_isequal(a, b, &eq);
  if (eq || a == b) {
    emit("M53-WAPCAPLET: fail distinct\n");
    return 1;
  }
  emit("M53-WAPCAPLET: ok distinct\n");

  /* ── lwc_string_data/length must round-trip the original bytes ── */
  if (lwc_string_length(a) != 7 ||
      memcmp(lwc_string_data(a), "display", 7) != 0) {
    emit("M53-WAPCAPLET: fail data\n");
    return 1;
  }
  emit("M53-WAPCAPLET: ok data\n");

  /* ── Case-insensitive comparison: "Hello" == "hELLO" but pointers differ ── */
  lwc_string *h1 = NULL, *h2 = NULL;
  if (lwc_intern_string("Hello", 5, &h1) != lwc_error_ok ||
      lwc_intern_string("hELLO", 5, &h2) != lwc_error_ok) {
    emit("M53-WAPCAPLET: fail intern\n");
    return 1;
  }
  bool caseless = false, exact = true;
  lwc_string_caseless_isequal(h1, h2, &caseless);
  lwc_string_isequal(h1, h2, &exact);
  if (!caseless || exact || h1 == h2) {
    emit("M53-WAPCAPLET: fail caseless\n");
    return 1;
  }
  emit("M53-WAPCAPLET: ok caseless\n");

  /* ── lwc_string_tolower must produce the lowercased form ── */
  if (lwc_string_tolower(h1, &upper) != lwc_error_ok || upper == NULL ||
      lwc_string_length(upper) != 5 ||
      memcmp(lwc_string_data(upper), "hello", 5) != 0) {
    emit("M53-WAPCAPLET: fail tolower\n");
    return 1;
  }
  emit("M53-WAPCAPLET: ok tolower\n");

  /* ── Reference counting: unref everything we interned ── */
  lwc_string_unref(a);
  lwc_string_unref(a2);
  lwc_string_unref(b);
  lwc_string_unref(h1);
  lwc_string_unref(h2);
  lwc_string_unref(upper);

  emit("M53-WAPCAPLET: done\n");
  return 0;
}
