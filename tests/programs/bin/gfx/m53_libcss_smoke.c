/* M53 libcss smoke: prove the ported NetSurf CSS engine works on b1nix. libcss
 * lexes/parses CSS and runs the cascade/selection that gives the DOM its
 * computed styles. It interns through libwapcaplet and parses input through
 * libparserutils (both already ported).
 *
 * This is the real engine end to end, nothing faked:
 *  - parse a stylesheet "p { color:#ff0000; display:block }" to completion,
 *  - build a selection context and run css_select_style over a one-node
 *    "document" (a libwapcaplet string holding the element name "p"),
 *  - read back the COMPUTED color (must be opaque red, proving #rrggbb parsing,
 *    the 0xAARRGGBB packing, and the cascade) and the COMPUTED display (block).
 * The select handler is the canonical single-node implementation from libcss's
 * own example1.c. Markers (M53-LIBCSS: ...) consumed by smoke.sh. */

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <libcss/libcss.h>

#define UNUSED(x) ((x) = (x))

static void emit(const char *s) { write(1, s, strlen(s)); }

/* ── Select handler: the document tree is a single node, a libwapcaplet string
 *    holding the element name. Everything but the name returns empty/false. ── */

static css_error resolve_url(void *pw, const char *base, lwc_string *rel,
                             lwc_string **abs) {
  UNUSED(pw);
  UNUSED(base);
  *abs = lwc_string_ref(rel);
  return CSS_OK;
}

static css_error node_name(void *pw, void *n, css_qname *qname) {
  lwc_string *node = n;
  UNUSED(pw);
  qname->name = lwc_string_ref(node);
  return CSS_OK;
}
static css_error node_classes(void *pw, void *n, lwc_string ***classes,
                              uint32_t *n_classes) {
  UNUSED(pw); UNUSED(n);
  *classes = NULL;
  *n_classes = 0;
  return CSS_OK;
}
static css_error node_id(void *pw, void *n, lwc_string **id) {
  UNUSED(pw); UNUSED(n);
  *id = NULL;
  return CSS_OK;
}
static css_error named_ancestor_node(void *pw, void *n, const css_qname *qname,
                                     void **ancestor) {
  UNUSED(pw); UNUSED(n); UNUSED(qname);
  *ancestor = NULL;
  return CSS_OK;
}
static css_error named_parent_node(void *pw, void *n, const css_qname *qname,
                                   void **parent) {
  UNUSED(pw); UNUSED(n); UNUSED(qname);
  *parent = NULL;
  return CSS_OK;
}
static css_error named_sibling_node(void *pw, void *n, const css_qname *qname,
                                    void **sibling) {
  UNUSED(pw); UNUSED(n); UNUSED(qname);
  *sibling = NULL;
  return CSS_OK;
}
static css_error named_generic_sibling_node(void *pw, void *n,
                                            const css_qname *qname,
                                            void **sibling) {
  UNUSED(pw); UNUSED(n); UNUSED(qname);
  *sibling = NULL;
  return CSS_OK;
}
static css_error parent_node(void *pw, void *n, void **parent) {
  UNUSED(pw); UNUSED(n);
  *parent = NULL;
  return CSS_OK;
}
static css_error sibling_node(void *pw, void *n, void **sibling) {
  UNUSED(pw); UNUSED(n);
  *sibling = NULL;
  return CSS_OK;
}
static css_error node_has_name(void *pw, void *n, const css_qname *qname,
                               bool *match) {
  lwc_string *node = n;
  UNUSED(pw);
  assert(lwc_string_caseless_isequal(node, qname->name, match) ==
         lwc_error_ok);
  return CSS_OK;
}
static css_error node_has_class(void *pw, void *n, lwc_string *name,
                                bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(name);
  *match = false;
  return CSS_OK;
}
static css_error node_has_id(void *pw, void *n, lwc_string *name, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(name);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute(void *pw, void *n, const css_qname *qname,
                                    bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_equal(void *pw, void *n,
                                          const css_qname *qname,
                                          lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_dashmatch(void *pw, void *n,
                                              const css_qname *qname,
                                              lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_includes(void *pw, void *n,
                                             const css_qname *qname,
                                             lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_prefix(void *pw, void *n,
                                           const css_qname *qname,
                                           lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_suffix(void *pw, void *n,
                                           const css_qname *qname,
                                           lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_has_attribute_substring(void *pw, void *n,
                                              const css_qname *qname,
                                              lwc_string *value, bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(qname); UNUSED(value);
  *match = false;
  return CSS_OK;
}
static css_error node_is_root(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = true; /* our single node is the root */
  return CSS_OK;
}
static css_error node_count_siblings(void *pw, void *n, bool same_name,
                                     bool after, int32_t *count) {
  UNUSED(pw); UNUSED(n); UNUSED(same_name); UNUSED(after);
  *count = 0;
  return CSS_OK;
}
static css_error node_is_empty(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = true;
  return CSS_OK;
}
static css_error node_is_link(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_visited(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_hover(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_active(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_focus(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_enabled(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_disabled(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_checked(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_target(void *pw, void *n, bool *match) {
  UNUSED(pw); UNUSED(n);
  *match = false;
  return CSS_OK;
}
static css_error node_is_lang(void *pw, void *n, lwc_string *lang,
                              bool *match) {
  UNUSED(pw); UNUSED(n); UNUSED(lang);
  *match = false;
  return CSS_OK;
}
static css_error node_presentational_hint(void *pw, void *node,
                                          uint32_t *nhints, css_hint **hints) {
  UNUSED(pw); UNUSED(node);
  *nhints = 0;
  *hints = NULL;
  return CSS_OK;
}
static css_select_handler select_handler; /* forward; set in main-init below */
static css_error ua_default_for_property(void *pw, uint32_t property,
                                         css_hint *hint) {
  UNUSED(pw);
  if (property == CSS_PROP_COLOR) {
    hint->data.color = 0x00000000;
    hint->status = CSS_COLOR_COLOR;
  } else if (property == CSS_PROP_FONT_FAMILY) {
    hint->data.strings = NULL;
    hint->status = CSS_FONT_FAMILY_SANS_SERIF;
  } else if (property == CSS_PROP_QUOTES) {
    hint->data.strings = NULL;
    hint->status = CSS_QUOTES_NONE;
  } else if (property == CSS_PROP_VOICE_FAMILY) {
    hint->data.strings = NULL;
    hint->status = 0;
  } else {
    return CSS_INVALID;
  }
  return CSS_OK;
}
static css_error set_libcss_node_data(void *pw, void *n,
                                      void *libcss_node_data) {
  UNUSED(pw);
  /* We don't store it, so ensure node data gets deleted. */
  css_libcss_node_data_handler(&select_handler, CSS_NODE_DELETED, pw, n, NULL,
                               libcss_node_data);
  return CSS_OK;
}
static css_error get_libcss_node_data(void *pw, void *n,
                                      void **libcss_node_data) {
  UNUSED(pw); UNUSED(n);
  *libcss_node_data = NULL;
  return CSS_OK;
}

static css_select_handler select_handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    node_name,
    node_classes,
    node_id,
    named_ancestor_node,
    named_parent_node,
    named_sibling_node,
    named_generic_sibling_node,
    parent_node,
    sibling_node,
    node_has_name,
    node_has_class,
    node_has_id,
    node_has_attribute,
    node_has_attribute_equal,
    node_has_attribute_dashmatch,
    node_has_attribute_includes,
    node_has_attribute_prefix,
    node_has_attribute_suffix,
    node_has_attribute_substring,
    node_is_root,
    node_count_siblings,
    node_is_empty,
    node_is_link,
    node_is_visited,
    node_is_hover,
    node_is_active,
    node_is_focus,
    node_is_enabled,
    node_is_disabled,
    node_is_checked,
    node_is_target,
    node_is_lang,
    node_presentational_hint,
    ua_default_for_property,
    set_libcss_node_data,
    get_libcss_node_data,
};

static css_unit_ctx unit_len_ctx = {
    .viewport_width = 800 * (1 << CSS_RADIX_POINT),
    .viewport_height = 600 * (1 << CSS_RADIX_POINT),
    .font_size_default = 16 * (1 << CSS_RADIX_POINT),
    .font_size_minimum = 6 * (1 << CSS_RADIX_POINT),
    .device_dpi = 96 * (1 << CSS_RADIX_POINT),
    .root_style = NULL,
    .pw = NULL,
    .measure = NULL,
};

int main(void) {
  emit("M53-LIBCSS: start\n");

  css_stylesheet_params params;
  memset(&params, 0, sizeof(params));
  params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
  params.level = CSS_LEVEL_21;
  params.charset = "UTF-8";
  params.url = "b1nix://test";
  params.title = "test";
  params.allow_quirks = false;
  params.inline_style = false;
  params.resolve = resolve_url;

  css_stylesheet *sheet = NULL;
  if (css_stylesheet_create(&params, &sheet) != CSS_OK || sheet == NULL) {
    emit("M53-LIBCSS: fail create\n");
    return 1;
  }
  emit("M53-LIBCSS: ok create\n");

  static const char data[] = "p { color: #ff0000; display: block; }";
  css_error code = css_stylesheet_append_data(sheet, (const uint8_t *)data,
                                              sizeof(data) - 1);
  if (code != CSS_OK && code != CSS_NEEDDATA) {
    emit("M53-LIBCSS: fail append\n");
    return 1;
  }
  if (css_stylesheet_data_done(sheet) != CSS_OK) {
    emit("M53-LIBCSS: fail parse\n");
    return 1;
  }
  emit("M53-LIBCSS: ok parse\n");

  css_select_ctx *ctx = NULL;
  if (css_select_ctx_create(&ctx) != CSS_OK ||
      css_select_ctx_append_sheet(ctx, sheet, CSS_ORIGIN_AUTHOR, NULL) !=
          CSS_OK) {
    emit("M53-LIBCSS: fail ctx\n");
    return 1;
  }
  emit("M53-LIBCSS: ok ctx\n");

  /* Our one-node document: a libwapcaplet string "p". */
  lwc_string *elem = NULL;
  if (lwc_intern_string("p", 1, &elem) != lwc_error_ok) {
    emit("M53-LIBCSS: fail intern\n");
    return 1;
  }

  css_media media;
  memset(&media, 0, sizeof(media));
  media.type = CSS_MEDIA_SCREEN;

  css_select_results *style = NULL;
  if (css_select_style(ctx, elem, &unit_len_ctx, &media, NULL, &select_handler,
                       0, &style) != CSS_OK ||
      style == NULL) {
    emit("M53-LIBCSS: fail select\n");
    return 1;
  }
  emit("M53-LIBCSS: ok select\n");

  const css_computed_style *cs = style->styles[CSS_PSEUDO_ELEMENT_NONE];

  /* color: #ff0000 → opaque red, packed 0xAARRGGBB. */
  css_color color = 0;
  uint8_t ctype = css_computed_color(cs, &color);
  if (ctype != CSS_COLOR_COLOR || (color & 0x00ffffff) != 0x00ff0000 ||
      (color >> 24) != 0xff) {
    emit("M53-LIBCSS: fail color\n");
    return 1;
  }
  emit("M53-LIBCSS: ok color\n");

  /* display: block. */
  uint8_t disp = css_computed_display(cs, false);
  if (disp != CSS_DISPLAY_BLOCK) {
    emit("M53-LIBCSS: fail display\n");
    return 1;
  }
  emit("M53-LIBCSS: ok display\n");

  css_select_results_destroy(style);
  lwc_string_unref(elem);
  css_select_ctx_destroy(ctx);
  css_stylesheet_destroy(sheet);

  emit("M53-LIBCSS: done\n");
  return 0;
}
