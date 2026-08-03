/* M51: verify the ported Fontconfig scans a font directory and matches a family
 * name to the bundled B1nix Mono font. Uses an in-memory config (no reliance on
 * /etc/fonts). */
#include <fontconfig/fontconfig.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

static const char *CONF =
    "<?xml version=\"1.0\"?>\n"
    "<fontconfig>\n"
    "  <dir>/share/fonts</dir>\n"
    "  <cachedir>/tmp/fontcache</cachedir>\n"
    "</fontconfig>\n";

int main(void) {
  FcConfig *cfg = FcConfigCreate();
  if (!cfg)
    return fail("M51-GFX: fail fontconfig (create)\n");
  if (!FcConfigParseAndLoadFromMemory(cfg, (const FcChar8 *)CONF, FcTrue))
    return fail("M51-GFX: fail fontconfig (parse)\n");
  if (!FcConfigBuildFonts(cfg))
    return fail("M51-GFX: fail fontconfig (build)\n");

  FcFontSet *sys = FcConfigGetFonts(cfg, FcSetSystem);
  if (!sys || sys->nfont < 1)
    return fail("M51-GFX: fail fontconfig (scan)\n");

  /* Match the family and confirm the resolved file is our bundled font. */
  FcPattern *pat = FcNameParse((const FcChar8 *)"B1nix Mono");
  FcConfigSubstitute(cfg, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res;
  FcPattern *m = FcFontMatch(cfg, pat, &res);
  if (!m)
    return fail("M51-GFX: fail fontconfig (match)\n");
  FcChar8 *file = 0;
  int ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file &&
           strstr((const char *)file, "B1nixMono") != 0;

  FcPatternDestroy(m);
  FcPatternDestroy(pat);
  FcConfigDestroy(cfg);
  FcFini();
  if (!ok)
    return fail("M51-GFX: fail fontconfig (file)\n");
  mark("M51-GFX: ok fontconfig\n");
  return 0;
}
