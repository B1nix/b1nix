/* M53 libhubbub smoke: prove the ported NetSurf HTML5 tokeniser works on b1nix.
 * libhubbub is the browser's HTML parser; it feeds the DOM (libdom) the token
 * stream produced here. It parses input through the already-ported
 * libparserutils. This drives the public parser in TOKEN_HANDLER mode (the
 * tokeniser, bypassing the tree builder) over a small real document and checks
 * the emitted token stream — nothing faked.
 *
 * Verified:
 *  - a DOCTYPE token with name "html",
 *  - START_TAG tokens for html/body/p, the <p> carrying class="x",
 *  - CHARACTER tokens spelling the body text "Hi",
 *  - matching END_TAG tokens,
 *  - an EOF token after hubbub_parser_completed().
 * Markers (M53-HUBBUB: ...) consumed by smoke.sh. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <hubbub/hubbub.h>
#include <hubbub/parser.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

struct counters {
  int doctype_html;       /* DOCTYPE token with name "html" */
  int start_tags;         /* total start tags */
  int end_tags;           /* total end tags */
  int saw_p_with_class_x; /* <p class="x"> */
  int eof;                /* EOF token */
  char text[32];          /* accumulated CHARACTER data */
  size_t text_len;
};

static int str_is(const hubbub_string *s, const char *lit) {
  size_t n = strlen(lit);
  return s->len == n && memcmp(s->ptr, lit, n) == 0;
}

static hubbub_error on_token(const hubbub_token *token, void *pw) {
  struct counters *c = pw;
  switch (token->type) {
  case HUBBUB_TOKEN_DOCTYPE:
    if (str_is(&token->data.doctype.name, "html"))
      c->doctype_html = 1;
    break;
  case HUBBUB_TOKEN_START_TAG:
    c->start_tags++;
    if (str_is(&token->data.tag.name, "p") &&
        token->data.tag.n_attributes == 1 &&
        str_is(&token->data.tag.attributes[0].name, "class") &&
        str_is(&token->data.tag.attributes[0].value, "x"))
      c->saw_p_with_class_x = 1;
    break;
  case HUBBUB_TOKEN_END_TAG:
    c->end_tags++;
    break;
  case HUBBUB_TOKEN_CHARACTER:
    for (size_t i = 0; i < token->data.character.len &&
                       c->text_len < sizeof(c->text) - 1;
         i++)
      c->text[c->text_len++] = (char)token->data.character.ptr[i];
    break;
  case HUBBUB_TOKEN_EOF:
    c->eof = 1;
    break;
  default:
    break;
  }
  return HUBBUB_OK;
}

int main(void) {
  emit("M53-HUBBUB: start\n");

  hubbub_parser *parser = NULL;
  if (hubbub_parser_create("UTF-8", true, &parser) != HUBBUB_OK ||
      parser == NULL) {
    emit("M53-HUBBUB: fail create\n");
    return 1;
  }
  emit("M53-HUBBUB: ok create\n");

  struct counters c;
  memset(&c, 0, sizeof(c));

  hubbub_parser_optparams params;
  params.token_handler.handler = on_token;
  params.token_handler.pw = &c;
  if (hubbub_parser_setopt(parser, HUBBUB_PARSER_TOKEN_HANDLER, &params) !=
      HUBBUB_OK) {
    emit("M53-HUBBUB: fail setopt\n");
    hubbub_parser_destroy(parser);
    return 1;
  }

  static const char html[] =
      "<!DOCTYPE html><html><body><p class=\"x\">Hi</p></body></html>";
  if (hubbub_parser_parse_chunk(parser, (const uint8_t *)html,
                                sizeof(html) - 1) != HUBBUB_OK) {
    emit("M53-HUBBUB: fail parse\n");
    hubbub_parser_destroy(parser);
    return 1;
  }
  if (hubbub_parser_completed(parser) != HUBBUB_OK) {
    emit("M53-HUBBUB: fail complete\n");
    hubbub_parser_destroy(parser);
    return 1;
  }
  emit("M53-HUBBUB: ok parse\n");

  hubbub_parser_destroy(parser);

  /* ── Verify the token stream ── */
  if (!c.doctype_html) {
    emit("M53-HUBBUB: fail doctype\n");
    return 1;
  }
  emit("M53-HUBBUB: ok doctype\n");

  /* html, body, p — at minimum, and matching end tags */
  if (c.start_tags < 3 || c.end_tags < 3) {
    emit("M53-HUBBUB: fail tags\n");
    return 1;
  }
  emit("M53-HUBBUB: ok tags\n");

  if (!c.saw_p_with_class_x) {
    emit("M53-HUBBUB: fail attribute\n");
    return 1;
  }
  emit("M53-HUBBUB: ok attribute\n");

  c.text[c.text_len] = '\0';
  if (strcmp(c.text, "Hi") != 0) {
    emit("M53-HUBBUB: fail text\n");
    return 1;
  }
  emit("M53-HUBBUB: ok text\n");

  if (!c.eof) {
    emit("M53-HUBBUB: fail eof\n");
    return 1;
  }
  emit("M53-HUBBUB: ok eof\n");

  emit("M53-HUBBUB: done\n");
  return 0;
}
