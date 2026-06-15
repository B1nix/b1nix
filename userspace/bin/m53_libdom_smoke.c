/* M53 libdom smoke: prove the ported NetSurf DOM works on b1nix. libdom builds
 * the document tree the browser lays out and styles; here we drive its libhubbub
 * binding (HTML -> DOM) over a real in-memory document and then navigate/query
 * the resulting tree. All three already-ported lower layers are exercised
 * transitively (libhubbub tokenises, libparserutils streams, libwapcaplet
 * interns the names). Nothing faked:
 *  - parse "<html><body><p id=x>Hi</p></body></html>" into a dom_document,
 *  - the document element is named HTML,
 *  - getElementsByTagName("p") returns exactly one node named P,
 *  - that node's id attribute is "x",
 *  - its text content is "Hi".
 * Markers (M53-LIBDOM: ...) consumed by smoke.sh. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

/* Compare a dom_string's bytes to a C literal. */
static int dstr_is(dom_string *s, const char *lit) {
  if (s == NULL)
    return 0;
  size_t n = strlen(lit);
  return dom_string_byte_length(s) == n &&
         memcmp(dom_string_data(s), lit, n) == 0;
}

int main(void) {
  emit("M53-LIBDOM: start\n");

  dom_hubbub_parser_params params;
  memset(&params, 0, sizeof(params));
  params.enc = NULL;
  params.fix_enc = true;
  params.enable_script = false;
  params.msg = NULL;
  params.script = NULL;
  params.ctx = NULL;
  params.daf = NULL;

  dom_hubbub_parser *parser = NULL;
  dom_document *doc = NULL;
  if (dom_hubbub_parser_create(&params, &parser, &doc) != DOM_HUBBUB_OK ||
      parser == NULL) {
    emit("M53-LIBDOM: fail create\n");
    return 1;
  }
  emit("M53-LIBDOM: ok create\n");

  static const char html[] =
      "<html><body><p id=\"x\">Hi</p></body></html>";
  if (dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html,
                                    sizeof(html) - 1) != DOM_HUBBUB_OK ||
      dom_hubbub_parser_completed(parser) != DOM_HUBBUB_OK) {
    emit("M53-LIBDOM: fail parse\n");
    dom_hubbub_parser_destroy(parser);
    return 1;
  }
  dom_hubbub_parser_destroy(parser);
  if (doc == NULL) {
    emit("M53-LIBDOM: fail parse\n");
    return 1;
  }
  emit("M53-LIBDOM: ok parse\n");

  /* ── Document element must be <html> ── */
  dom_element *root = NULL;
  if (dom_document_get_document_element(doc, &root) != DOM_NO_ERR ||
      root == NULL) {
    emit("M53-LIBDOM: fail root\n");
    return 1;
  }
  dom_string *root_name = NULL;
  if (dom_node_get_node_name(root, &root_name) != DOM_NO_ERR ||
      !dstr_is(root_name, "HTML")) {
    emit("M53-LIBDOM: fail root\n");
    return 1;
  }
  dom_string_unref(root_name);
  emit("M53-LIBDOM: ok root\n");

  /* ── getElementsByTagName("p") → exactly one <p> ── */
  dom_string *p_tag = NULL;
  if (dom_string_create_interned((const uint8_t *)"p", 1, &p_tag) !=
      DOM_NO_ERR) {
    emit("M53-LIBDOM: fail intern\n");
    return 1;
  }
  dom_nodelist *plist = NULL;
  uint32_t plen = 0;
  if (dom_document_get_elements_by_tag_name(doc, p_tag, &plist) != DOM_NO_ERR ||
      plist == NULL || dom_nodelist_get_length(plist, &plen) != DOM_NO_ERR ||
      plen != 1) {
    emit("M53-LIBDOM: fail query\n");
    return 1;
  }
  emit("M53-LIBDOM: ok query\n");

  dom_node *p = NULL;
  if (dom_nodelist_item(plist, 0, &p) != DOM_NO_ERR || p == NULL) {
    emit("M53-LIBDOM: fail query\n");
    return 1;
  }
  dom_string *p_name = NULL;
  if (dom_node_get_node_name(p, &p_name) != DOM_NO_ERR ||
      !dstr_is(p_name, "P")) {
    emit("M53-LIBDOM: fail query\n");
    return 1;
  }
  dom_string_unref(p_name);

  /* ── The <p>'s id attribute is "x" ── */
  dom_string *id_name = NULL;
  dom_string *id_val = NULL;
  if (dom_string_create_interned((const uint8_t *)"id", 2, &id_name) !=
      DOM_NO_ERR) {
    emit("M53-LIBDOM: fail intern\n");
    return 1;
  }
  if (dom_element_get_attribute(p, id_name, &id_val) != DOM_NO_ERR ||
      !dstr_is(id_val, "x")) {
    emit("M53-LIBDOM: fail attribute\n");
    return 1;
  }
  dom_string_unref(id_val);
  dom_string_unref(id_name);
  emit("M53-LIBDOM: ok attribute\n");

  /* ── The <p>'s text content is "Hi" ── */
  dom_string *text = NULL;
  if (dom_node_get_text_content(p, &text) != DOM_NO_ERR ||
      !dstr_is(text, "Hi")) {
    emit("M53-LIBDOM: fail text\n");
    return 1;
  }
  dom_string_unref(text);
  emit("M53-LIBDOM: ok text\n");

  dom_node_unref(p);
  dom_string_unref(p_tag);
  dom_nodelist_unref(plist);
  dom_node_unref(root);
  dom_node_unref(doc);

  emit("M53-LIBDOM: done\n");
  return 0;
}
