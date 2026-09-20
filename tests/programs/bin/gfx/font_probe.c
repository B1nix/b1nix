/*
 * The three steps pango takes to turn a font name into something cairo can
 * draw with, done one at a time and reported separately.
 *
 * A compositor said only "file not found" about a font that fc-match resolves
 * and FreeType opens, which is three different subsystems ago from where the
 * message appears. Each step here prints what it produced, so the first one
 * that produces the wrong thing is named rather than inferred:
 *
 *   1. fontconfig matches a pattern for "monospace"
 *   2. the matched pattern carries a file name
 *   3. cairo builds a font face from that pattern, and reports its status
 *
 * Everything is reached through dlopen so this builds without any of those
 * libraries' headers and runs against exactly the shared objects the
 * compositor loads — which is the point: a static copy of fontconfig would be
 * answering a different question.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

typedef void *FcConfigP;
typedef void *FcPatternP;

/* Fontconfig, as much of it as this needs. */
typedef int (*fn_FcInit)(void);
typedef FcPatternP (*fn_FcNameParse)(const unsigned char *);
typedef int (*fn_FcConfigSubstitute)(FcConfigP, FcPatternP, int);
typedef void (*fn_FcDefaultSubstitute)(FcPatternP);
typedef FcPatternP (*fn_FcFontMatch)(FcConfigP, FcPatternP, int *);
typedef int (*fn_FcPatternGetString)(FcPatternP, const char *, int,
                                     unsigned char **);
typedef int (*fn_FcPatternGetInteger)(FcPatternP, const char *, int, int *);
typedef void *(*fn_FcFontSort)(FcConfigP, FcPatternP, int, void *, int *);
typedef FcPatternP (*fn_FcFontRenderPrepare)(FcConfigP, FcPatternP, FcPatternP);
struct fc_fontset { int nfont; int sfont; FcPatternP *fonts; };

/* What sway itself does: a pango layout on a cairo surface, measured. */
typedef void *(*fn_cairo_image_surface_create)(int, int, int);
typedef void *(*fn_cairo_create)(void *);
typedef void *(*fn_pango_cairo_create_layout)(void *);
typedef void *(*fn_pango_font_description_from_string)(const char *);
typedef void (*fn_pango_layout_set_font_description)(void *, void *);
typedef void (*fn_pango_layout_set_text)(void *, const char *, int);
typedef void (*fn_pango_layout_get_pixel_size)(void *, int *, int *);

/* cairo's fontconfig entry point, and the status of what it returns. */
typedef void *(*fn_cairo_ft_font_face_create_for_pattern)(FcPatternP);
typedef int (*fn_cairo_font_face_status)(void *);
typedef const char *(*fn_cairo_status_to_string)(int);

#define NEED(handle, type, name)                                              \
  type name = (type)dlsym(handle, #name);                                     \
  if (!name) {                                                                \
    printf("FONT-PROBE: fail dlsym %s\n", #name);                             \
    return 1;                                                                 \
  }

int main(int argc, char **argv) {
  const char *want = argc > 1 ? argv[1] : "monospace";
  void *fc = dlopen("libfontconfig.so.1", RTLD_NOW);
  void *cr = dlopen("libcairo.so.2", RTLD_NOW);

  if (!fc) {
    printf("FONT-PROBE: fail dlopen fontconfig: %s\n", dlerror());
    return 1;
  }
  if (!cr) {
    printf("FONT-PROBE: fail dlopen cairo: %s\n", dlerror());
    return 1;
  }

  NEED(fc, fn_FcInit, FcInit)
  NEED(fc, fn_FcNameParse, FcNameParse)
  NEED(fc, fn_FcConfigSubstitute, FcConfigSubstitute)
  NEED(fc, fn_FcDefaultSubstitute, FcDefaultSubstitute)
  NEED(fc, fn_FcFontMatch, FcFontMatch)
  NEED(fc, fn_FcPatternGetString, FcPatternGetString)
  NEED(fc, fn_FcPatternGetInteger, FcPatternGetInteger)
  NEED(fc, fn_FcFontSort, FcFontSort)
  NEED(fc, fn_FcFontRenderPrepare, FcFontRenderPrepare)
  NEED(cr, fn_cairo_ft_font_face_create_for_pattern,
       cairo_ft_font_face_create_for_pattern)
  NEED(cr, fn_cairo_font_face_status, cairo_font_face_status)
  NEED(cr, fn_cairo_status_to_string, cairo_status_to_string)

  if (!FcInit()) {
    printf("FONT-PROBE: fail FcInit\n");
    return 1;
  }
  printf("FONT-PROBE: ok fcinit\n");

  FcPatternP pat = FcNameParse((const unsigned char *)want);
  if (!pat) {
    printf("FONT-PROBE: fail FcNameParse\n");
    return 1;
  }
  /* 1 is FcMatchPattern — the value pango passes here. */
  FcConfigSubstitute(0, pat, 0);
  FcDefaultSubstitute(pat);

  int result = 0;
  FcPatternP match = FcFontMatch(0, pat, &result);
  if (!match) {
    printf("FONT-PROBE: fail FcFontMatch result=%d\n", result);
    return 1;
  }
  printf("FONT-PROBE: ok match result=%d\n", result);

  unsigned char *file = 0;
  int idx = -1;
  int rc = FcPatternGetString(match, "file", 0, &file);
  FcPatternGetInteger(match, "index", 0, &idx);
  printf("FONT-PROBE: file rc=%d idx=%d path=%s\n", rc, idx,
         (rc == 0 && file) ? (const char *)file : "(none)");

  void *face = cairo_ft_font_face_create_for_pattern(match);
  int st = face ? cairo_font_face_status(face) : -1;

  printf("FONT-PROBE: cairo face=%p status=%d (%s)\n", face, st,
         st >= 0 ? cairo_status_to_string(st) : "no face");
  printf(st == 0 ? "FONT-PROBE: ok cairo-face\n"
                 : "FONT-PROBE: fail cairo-face\n");

  /*
   * And now the way pango really does it.
   *
   * pango does not hand cairo the match; it sorts the fonts and prepares each
   * candidate against the request with FcFontRenderPrepare, which builds a new
   * pattern by merging the two. If the file element is lost in that merge, the
   * result is a pattern cairo cannot open — reported, three layers up, as
   * "file not found".
   */
  {
    int nsort = 0;
    struct fc_fontset *set = FcFontSort(0, pat, 1, 0, &nsort);

    if (!set || set->nfont < 1) {
      printf("FONT-PROBE: fail FcFontSort n=%d\n", set ? set->nfont : -1);
      return 1;
    }
    FcPatternP prepared = FcFontRenderPrepare(0, pat, set->fonts[0]);

    if (!prepared) {
      printf("FONT-PROBE: fail FcFontRenderPrepare\n");
      return 1;
    }
    unsigned char *pfile = 0;
    int prc = FcPatternGetString(prepared, "file", 0, &pfile);
    void *pface = cairo_ft_font_face_create_for_pattern(prepared);
    int pst = pface ? cairo_font_face_status(pface) : -1;

    printf("FONT-PROBE: prepared nfont=%d file rc=%d path=%s\n", set->nfont,
           prc, (prc == 0 && pfile) ? (const char *)pfile : "(none)");
    printf("FONT-PROBE: prepared cairo status=%d (%s)\n", pst,
           pst >= 0 ? cairo_status_to_string(pst) : "no face");
    printf(pst == 0 ? "FONT-PROBE: ok prepared-face\n"
                    : "FONT-PROBE: fail prepared-face\n");
    if (pst != 0)
      return 1;
  }
  /*
   * The measurement sway makes before it can lay out anything at all.
   *
   * Everything above is a step pango takes internally; this is the call sway
   * writes. If the pieces work and the whole does not, the fault is in how
   * pango puts them together — which is where the "no property named
   * 'pattern'" complaint comes from.
   */
  {
    void *pc = dlopen("libpangocairo-1.0.so.0", RTLD_NOW);

    if (!pc) {
      printf("FONT-PROBE: fail dlopen pangocairo: %s\n", dlerror());
    } else {
      NEED(cr, fn_cairo_image_surface_create, cairo_image_surface_create)
      NEED(cr, fn_cairo_create, cairo_create)
      NEED(pc, fn_pango_cairo_create_layout, pango_cairo_create_layout)
      NEED(pc, fn_pango_font_description_from_string,
           pango_font_description_from_string)
      NEED(pc, fn_pango_layout_set_font_description,
           pango_layout_set_font_description)
      NEED(pc, fn_pango_layout_set_text, pango_layout_set_text)
      NEED(pc, fn_pango_layout_get_pixel_size, pango_layout_get_pixel_size)

      void *surf = cairo_image_surface_create(0 /* ARGB32 */, 64, 64);
      void *ctx = cairo_create(surf);
      void *layout = pango_cairo_create_layout(ctx);
      /* The description sway ends up with, fractional size and all, when the
       * caller passes one — that is what its warning names. */
      void *desc = pango_font_description_from_string(
          argc > 2 ? argv[2] : "monospace 10");
      int w = -1, h = -1;

      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(layout, "b1nix", -1);
      pango_layout_get_pixel_size(layout, &w, &h);
      printf("FONT-PROBE: pango text %dx%d\n", w, h);
      printf((w > 0 && h > 0) ? "FONT-PROBE: ok pango-layout\n"
                              : "FONT-PROBE: fail pango-layout\n");
    }
  }

  /*
   * What sway does between opening its backend and measuring a font.
   *
   * Compiling an XKB keymap reads a few hundred small files and allocates
   * heavily while doing it. If the font path works before that and not after,
   * the damage is done here — and this probe can then be cut down until only
   * the offending call is left.
   */
  {
    void *xkb = dlopen("libxkbcommon.so.0", RTLD_NOW);

    if (!xkb) {
      printf("FONT-PROBE: no xkbcommon: %s\n", dlerror());
    } else {
      typedef void *(*fn_ctx_new)(int);
      typedef void *(*fn_keymap_new)(void *, const void *, int);
      typedef void (*fn_keymap_unref)(void *);
      fn_ctx_new xkb_context_new = (fn_ctx_new)dlsym(xkb, "xkb_context_new");
      fn_keymap_new xkb_keymap_new_from_names =
          (fn_keymap_new)dlsym(xkb, "xkb_keymap_new_from_names");
      fn_keymap_unref xkb_keymap_unref =
          (fn_keymap_unref)dlsym(xkb, "xkb_keymap_unref");

      if (xkb_context_new && xkb_keymap_new_from_names && xkb_keymap_unref) {
        void *c = xkb_context_new(0);
        void *km = c ? xkb_keymap_new_from_names(c, 0, 0) : 0;

        printf("FONT-PROBE: xkb keymap=%p\n", km);
        if (km)
          xkb_keymap_unref(km);
      }
    }

    /* The same measurement again, now that the keymap work has happened. */
    void *pc2 = dlopen("libpangocairo-1.0.so.0", RTLD_NOW);

    if (pc2) {
      fn_cairo_image_surface_create isc =
          (fn_cairo_image_surface_create)dlsym(cr, "cairo_image_surface_create");
      fn_cairo_create cc = (fn_cairo_create)dlsym(cr, "cairo_create");
      fn_pango_cairo_create_layout pcl =
          (fn_pango_cairo_create_layout)dlsym(pc2, "pango_cairo_create_layout");
      fn_pango_font_description_from_string pfd =
          (fn_pango_font_description_from_string)dlsym(
              pc2, "pango_font_description_from_string");
      fn_pango_layout_set_font_description plsfd =
          (fn_pango_layout_set_font_description)dlsym(
              pc2, "pango_layout_set_font_description");
      fn_pango_layout_set_text plst =
          (fn_pango_layout_set_text)dlsym(pc2, "pango_layout_set_text");
      fn_pango_layout_get_pixel_size plgps =
          (fn_pango_layout_get_pixel_size)dlsym(pc2,
                                               "pango_layout_get_pixel_size");
      int w = -1, h = -1;

      if (isc && cc && pcl && pfd && plsfd && plst && plgps) {
        void *layout = pcl(cc(isc(0, 64, 64)));

        plsfd(layout, pfd("monospace 10"));
        plst(layout, "b1nix", -1);
        plgps(layout, &w, &h);
      }
      printf("FONT-PROBE: pango after xkb %dx%d\n", w, h);
      printf((w > 0 && h > 0) ? "FONT-PROBE: ok pango-after-xkb\n"
                              : "FONT-PROBE: fail pango-after-xkb\n");
    }
  }

  /*
   * How many files this process may hold open at once.
   *
   * cairo answers "file not found" for a font it could not open, and running
   * out of descriptors is one way to fail to open a file that is plainly
   * there. A compositor holds far more of them than this probe does, so the
   * ceiling matters even when every step above succeeds.
   */
  {
    int held[4096];
    int n = 0;

    while (n < (int)(sizeof(held) / sizeof(held[0]))) {
      int f = open("/dev/null", O_RDONLY);

      if (f < 0)
        break;
      held[n++] = f;
    }
    printf("FONT-PROBE: descriptors %d then errno=%d\n", n, errno);
    for (int i = 0; i < n; i++)
      close(held[i]);
  }
  return st == 0 ? 0 : 1;
}
