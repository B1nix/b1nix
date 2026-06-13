/* M51 rung 7: verify the ported xkbcommon compiles a keymap from a string and
 * translates an evdev keycode into the right keysym. Ships its own tiny keymap
 * so there is no xkeyboard-config data dependency. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static void mark(const char *s) { write(1, s, strlen(s)); }
static int fail(const char *s) {
  mark(s);
  return 1;
}

/* Minimal but valid XKB keymap: physical key AC01 (evdev KEY_A=30 -> xkb
 * keycode 38) carries the 'a'/'A' symbols on a two-level (Shift) type. */
static const char KEYMAP[] =
    "xkb_keymap {\n"
    "  xkb_keycodes \"k\" { minimum = 8; maximum = 255; <AC01> = 38; };\n"
    "  xkb_types \"t\" {\n"
    "    virtual_modifiers NumLock;\n"
    "    type \"TWO_LEVEL\" {\n"
    "      modifiers = Shift;\n"
    "      map[Shift] = Level2;\n"
    "      level_name[Level1] = \"Base\";\n"
    "      level_name[Level2] = \"Shift\";\n"
    "    };\n"
    "  };\n"
    "  xkb_compatibility \"c\" { };\n"
    "  xkb_symbols \"s\" {\n"
    "    key <AC01> { type = \"TWO_LEVEL\", [ a, A ] };\n"
    "  };\n"
    "};\n";

int main(void) {
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!ctx)
    return fail("M51-GFX: fail xkb (context)\n");

  struct xkb_keymap *km = xkb_keymap_new_from_string(
      ctx, KEYMAP, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!km)
    return fail("M51-GFX: fail xkb (keymap)\n");

  struct xkb_state *st = xkb_state_new(km);
  if (!st)
    return fail("M51-GFX: fail xkb (state)\n");

  /* Base level: keycode 38 -> 'a'. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(st, 38);
  if (sym != XKB_KEY_a)
    return fail("M51-GFX: fail xkb (base-sym)\n");

  char name[16];
  if (xkb_keysym_get_name(sym, name, sizeof(name)) <= 0 || strcmp(name, "a"))
    return fail("M51-GFX: fail xkb (name)\n");

  /* Drive the Shift modifier directly; the same key now yields 'A'. */
  xkb_state_update_mask(st, 1u << 0 /* Shift */, 0, 0, 0, 0, 0);
  sym = xkb_state_key_get_one_sym(st, 38);
  if (sym != XKB_KEY_A)
    return fail("M51-GFX: fail xkb (shift-sym)\n");

  xkb_state_unref(st);
  xkb_keymap_unref(km);
  xkb_context_unref(ctx);
  mark("M51-GFX: ok xkbcommon\n");
  return 0;
}
