/* M32a PCRE2 smoke: prove the ported PCRE2 (8-bit) runtime compiles a pattern
 * and matches/anti-matches correctly on b1nix. Markers (M32-PCRE2: ...) are
 * consumed by tests/smoke.sh. */

#include <string.h>
#include <unistd.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

int main(void) {
  emit("M32-PCRE2: start\n");

  int errcode = 0;
  PCRE2_SIZE erroffset = 0;
  pcre2_code *re = pcre2_compile((PCRE2_SPTR) "^b1([0-9]+)x$",
                                 PCRE2_ZERO_TERMINATED, 0, &errcode,
                                 &erroffset, NULL);
  if (!re) {
    emit("M32-PCRE2: fail compile\n");
    return 1;
  }
  emit("M32-PCRE2: ok compile\n");

  pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
  if (!md) {
    emit("M32-PCRE2: fail match-data\n");
    pcre2_code_free(re);
    return 1;
  }

  /* Positive: "b1234x" matches and the captured group is "234". */
  const char *subject = "b1234x";
  int rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject), 0, 0, md, NULL);
  if (rc < 2) {
    emit("M32-PCRE2: fail match\n");
    return 1;
  }
  PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
  PCRE2_SIZE gs = ov[2], ge = ov[3];
  if (ge - gs != 3 || memcmp(subject + gs, "234", 3) != 0) {
    emit("M32-PCRE2: fail capture\n");
    return 1;
  }
  emit("M32-PCRE2: ok match\n");

  /* Negative: "nope" must not match. */
  const char *nope = "nope";
  rc = pcre2_match(re, (PCRE2_SPTR)nope, strlen(nope), 0, 0, md, NULL);
  if (rc != PCRE2_ERROR_NOMATCH) {
    emit("M32-PCRE2: fail nomatch\n");
    return 1;
  }
  emit("M32-PCRE2: ok nomatch\n");

  pcre2_match_data_free(md);
  pcre2_code_free(re);
  emit("M32-PCRE2: done\n");
  return 0;
}
