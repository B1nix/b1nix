#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#define RX_MAX_STATES 256
#define RX_MAX_CAPTURES 10

enum rx_type {
  RX_EMPTY,
  RX_CHAR,
  RX_DOT,
  RX_CLASS,
  RX_BOL,
  RX_EOL,
  RX_BACKREF,
  RX_SEQ,
  RX_ALT,
  RX_REPEAT,
  RX_GROUP
};

struct rx_node {
  enum rx_type type;
  struct rx_node *left;
  struct rx_node *right;
  int ch;
  int min;
  int max;
  int group;
  unsigned char cls[32];
  int negate;
};

struct rx_parser {
  const char *p;
  int extended;
  int error;
  int groups;
};

struct rx_state {
  const char *pos;
  regmatch_t caps[RX_MAX_CAPTURES];
};

static struct rx_node *rx_new(enum rx_type type) {
  struct rx_node *node = calloc(1, sizeof(*node));
  if (node)
    node->type = type;
  return node;
}

static void rx_free(struct rx_node *node) {
  if (!node)
    return;
  rx_free(node->left);
  rx_free(node->right);
  free(node);
}

static int rx_is_alt(struct rx_parser *ps) {
  return ps->extended ? ps->p[0] == '|' : ps->p[0] == '\\' && ps->p[1] == '|';
}

static int rx_is_close(struct rx_parser *ps) {
  return ps->extended ? ps->p[0] == ')' : ps->p[0] == '\\' && ps->p[1] == ')';
}

static void rx_set_class(unsigned char cls[32], unsigned char c) {
  cls[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static void rx_add_named_class(unsigned char cls[32], const char *name,
                               size_t length) {
  for (int c = 0; c < 256; c++) {
    int match = 0;
    if (length == 5 && memcmp(name, "alpha", 5) == 0) match = isalpha(c);
    else if (length == 5 && memcmp(name, "alnum", 5) == 0) match = isalnum(c);
    else if (length == 5 && memcmp(name, "digit", 5) == 0) match = isdigit(c);
    else if (length == 5 && memcmp(name, "space", 5) == 0) match = isspace(c);
    else if (length == 5 && memcmp(name, "lower", 5) == 0) match = islower(c);
    else if (length == 5 && memcmp(name, "upper", 5) == 0) match = isupper(c);
    else if (length == 6 && memcmp(name, "xdigit", 6) == 0) match = isxdigit(c);
    else if (length == 5 && memcmp(name, "blank", 5) == 0)
      match = c == ' ' || c == '\t';
    else if (length == 5 && memcmp(name, "cntrl", 5) == 0) match = iscntrl(c);
    else if (length == 5 && memcmp(name, "graph", 5) == 0) match = isgraph(c);
    else if (length == 5 && memcmp(name, "print", 5) == 0) match = isprint(c);
    else if (length == 5 && memcmp(name, "punct", 5) == 0) match = ispunct(c);
    if (match)
      rx_set_class(cls, (unsigned char)c);
  }
}

static struct rx_node *rx_parse_alt(struct rx_parser *ps);

static struct rx_node *rx_parse_class(struct rx_parser *ps) {
  struct rx_node *node = rx_new(RX_CLASS);
  if (!node)
    return NULL;
  ps->p++;
  if (*ps->p == '^') {
    node->negate = 1;
    ps->p++;
  }
  int first = 1;
  while (*ps->p && (*ps->p != ']' || first)) {
    first = 0;
    if (ps->p[0] == '[' && ps->p[1] == ':') {
      const char *end = strstr(ps->p + 2, ":]");
      if (end) {
        rx_add_named_class(node->cls, ps->p + 2, (size_t)(end - ps->p - 2));
        ps->p = end + 2;
        continue;
      }
    }
    unsigned char start = (unsigned char)*ps->p++;
    if (start == '\\' && *ps->p)
      start = (unsigned char)*ps->p++;
    if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
      ps->p++;
      unsigned char end = (unsigned char)*ps->p++;
      if (end == '\\' && *ps->p)
        end = (unsigned char)*ps->p++;
      if (start > end) {
        ps->error = REG_BADPAT;
        rx_free(node);
        return NULL;
      }
      for (int c = start; c <= end; c++)
        rx_set_class(node->cls, (unsigned char)c);
    } else {
      rx_set_class(node->cls, start);
    }
  }
  if (*ps->p != ']') {
    ps->error = REG_BADPAT;
    rx_free(node);
    return NULL;
  }
  ps->p++;
  return node;
}

static struct rx_node *rx_parse_atom(struct rx_parser *ps) {
  if (!*ps->p || rx_is_alt(ps) || rx_is_close(ps))
    return rx_new(RX_EMPTY);
  if (*ps->p == '^') {
    ps->p++;
    return rx_new(RX_BOL);
  }
  if (*ps->p == '$') {
    ps->p++;
    return rx_new(RX_EOL);
  }
  if (*ps->p == '.') {
    ps->p++;
    return rx_new(RX_DOT);
  }
  if (*ps->p == '[')
    return rx_parse_class(ps);

  int group_open = ps->extended ? *ps->p == '('
                                : ps->p[0] == '\\' && ps->p[1] == '(';
  if (group_open) {
    ps->p += ps->extended ? 1 : 2;
    int group = ++ps->groups;
    struct rx_node *child = rx_parse_alt(ps);
    if (!rx_is_close(ps)) {
      ps->error = REG_BADPAT;
      rx_free(child);
      return NULL;
    }
    ps->p += ps->extended ? 1 : 2;
    struct rx_node *node = rx_new(RX_GROUP);
    if (!node) {
      rx_free(child);
      return NULL;
    }
    node->left = child;
    node->group = group;
    return node;
  }

  struct rx_node *node;
  if (*ps->p == '\\' && ps->p[1]) {
    ps->p++;
    if (*ps->p >= '1' && *ps->p <= '9') {
      node = rx_new(RX_BACKREF);
      if (node)
        node->group = *ps->p++ - '0';
      return node;
    }
  }
  node = rx_new(RX_CHAR);
  if (node)
    node->ch = (unsigned char)*ps->p++;
  return node;
}

static struct rx_node *rx_parse_piece(struct rx_parser *ps) {
  struct rx_node *atom = rx_parse_atom(ps);
  if (!atom)
    return NULL;
  int min = -1, max = -1, consumed = 0;
  if (*ps->p == '*') {
    min = 0;
    max = -1;
    consumed = 1;
  } else if (ps->extended && *ps->p == '+') {
    min = 1;
    max = -1;
    consumed = 1;
  } else if (ps->extended && *ps->p == '?') {
    min = 0;
    max = 1;
    consumed = 1;
  } else if (!ps->extended && ps->p[0] == '\\' && ps->p[1] == '+') {
    min = 1;
    max = -1;
    consumed = 2;
  } else if (!ps->extended && ps->p[0] == '\\' && ps->p[1] == '?') {
    min = 0;
    max = 1;
    consumed = 2;
  } else {
    int is_interval = 0;
    const char *start_ptr = ps->p;
    if (ps->extended && *start_ptr == '{') {
      is_interval = 1;
      start_ptr++;
    } else if (!ps->extended && start_ptr[0] == '\\' && start_ptr[1] == '{') {
      is_interval = 2;
      start_ptr += 2;
    }

    if (is_interval) {
      const char *curr = start_ptr;
      int m = 0, n = -1, has_comma = 0;
      if (curr[0] >= '0' && curr[0] <= '9') {
        while (curr[0] >= '0' && curr[0] <= '9') {
          m = m * 10 + (curr[0] - '0');
          curr++;
        }
        if (*curr == ',') {
          has_comma = 1;
          curr++;
          if (curr[0] >= '0' && curr[0] <= '9') {
            n = 0;
            while (curr[0] >= '0' && curr[0] <= '9') {
              n = n * 10 + (curr[0] - '0');
              curr++;
            }
          }
        } else {
          n = m;
        }

        int closed = 0;
        if (is_interval == 1 && *curr == '}') {
          closed = 1;
          curr++;
        } else if (is_interval == 2 && curr[0] == '\\' && curr[1] == '}') {
          closed = 2;
          curr += 2;
        }

        if (closed) {
          min = m;
          max = n;
          consumed = (int)(curr - ps->p);
        }
      }
    }
  }

  if (!consumed)
    return atom;
  ps->p += consumed;
  struct rx_node *node = rx_new(RX_REPEAT);
  if (!node) {
    rx_free(atom);
    return NULL;
  }
  node->left = atom;
  node->min = min;
  node->max = max;
  return node;
}

static struct rx_node *rx_parse_seq(struct rx_parser *ps) {
  struct rx_node *seq = NULL;
  while (*ps->p && !rx_is_alt(ps) && !rx_is_close(ps)) {
    struct rx_node *piece = rx_parse_piece(ps);
    if (!piece) {
      rx_free(seq);
      return NULL;
    }
    if (!seq) {
      seq = piece;
    } else {
      struct rx_node *node = rx_new(RX_SEQ);
      if (!node) {
        rx_free(seq);
        rx_free(piece);
        return NULL;
      }
      node->left = seq;
      node->right = piece;
      seq = node;
    }
  }
  return seq ? seq : rx_new(RX_EMPTY);
}

static struct rx_node *rx_parse_alt(struct rx_parser *ps) {
  struct rx_node *left = rx_parse_seq(ps);
  while (left && rx_is_alt(ps)) {
    ps->p += ps->extended ? 1 : 2;
    struct rx_node *right = rx_parse_seq(ps);
    struct rx_node *node = rx_new(RX_ALT);
    if (!right || !node) {
      rx_free(left);
      rx_free(right);
      rx_free(node);
      return NULL;
    }
    node->left = left;
    node->right = right;
    left = node;
  }
  return left;
}

static int rx_same_state(const struct rx_state *a, const struct rx_state *b) {
  return a->pos == b->pos && memcmp(a->caps, b->caps, sizeof(a->caps)) == 0;
}

static void rx_push(struct rx_state out[], int *count,
                    const struct rx_state *state) {
  for (int i = 0; i < *count; i++)
    if (rx_same_state(&out[i], state))
      return;
  if (*count < RX_MAX_STATES)
    out[(*count)++] = *state;
}

static int rx_eval(const struct rx_node *node, const char *start,
                   const char *end, int cflags, int eflags,
                   const struct rx_state *in, struct rx_state out[]);

static int rx_eval_repeat(const struct rx_node *node, const char *start,
                          const char *end, int cflags, int eflags,
                          const struct rx_state *in, struct rx_state out[]) {
  struct rx_state levels[RX_MAX_STATES];
  int level_count = 1;
  levels[0] = *in;
  int out_count = 0;
  int repeats = 0;
  if (node->min == 0)
    rx_push(out, &out_count, in);
  while (level_count && (node->max < 0 || repeats < node->max)) {
    struct rx_state next[RX_MAX_STATES];
    int next_count = 0;
    for (int i = 0; i < level_count; i++) {
      struct rx_state matches[RX_MAX_STATES];
      int n = rx_eval(node->left, start, end, cflags, eflags, &levels[i], matches);
      for (int j = 0; j < n; j++)
        if (matches[j].pos != levels[i].pos)
          rx_push(next, &next_count, &matches[j]);
    }
    repeats++;
    if (repeats >= node->min)
      for (int i = 0; i < next_count; i++)
        rx_push(out, &out_count, &next[i]);
    memcpy(levels, next, (size_t)next_count * sizeof(*levels));
    level_count = next_count;
  }
  return out_count;
}

static int rx_eval(const struct rx_node *node, const char *start,
                   const char *end, int cflags, int eflags,
                   const struct rx_state *in, struct rx_state out[]) {
  int count = 0;
  struct rx_state state = *in;
  switch (node->type) {
  case RX_EMPTY:
    out[0] = state;
    return 1;
  case RX_CHAR:
    if (state.pos < end) {
      int a = (unsigned char)*state.pos;
      int b = node->ch;
      if (cflags & REG_ICASE) a = tolower(a), b = tolower(b);
      if (a == b) {
        state.pos++;
        out[0] = state;
        return 1;
      }
    }
    return 0;
  case RX_DOT:
    if (state.pos < end && (!(cflags & REG_NEWLINE) || *state.pos != '\n')) {
      state.pos++;
      out[0] = state;
      return 1;
    }
    return 0;
  case RX_CLASS:
    if (state.pos < end) {
      unsigned char c = (unsigned char)*state.pos;
      int match = (node->cls[c >> 3] >> (c & 7)) & 1;
      if ((cflags & REG_ICASE) && !match) {
        unsigned char lower = (unsigned char)tolower(c);
        unsigned char upper = (unsigned char)toupper(c);
        match = ((node->cls[lower >> 3] >> (lower & 7)) & 1) ||
                ((node->cls[upper >> 3] >> (upper & 7)) & 1);
      }
      if (node->negate)
        match = !match;
      if (match && (!(cflags & REG_NEWLINE) || *state.pos != '\n')) {
        state.pos++;
        out[0] = state;
        return 1;
      }
    }
    return 0;
  case RX_BOL:
    if ((state.pos == start && !(eflags & REG_NOTBOL)) ||
        ((cflags & REG_NEWLINE) && state.pos > start && state.pos[-1] == '\n')) {
      out[0] = state;
      return 1;
    }
    return 0;
  case RX_EOL:
    if ((state.pos == end && !(eflags & REG_NOTEOL)) ||
        ((cflags & REG_NEWLINE) && state.pos < end && *state.pos == '\n')) {
      out[0] = state;
      return 1;
    }
    return 0;
  case RX_BACKREF: {
    int group = node->group;
    if (group >= RX_MAX_CAPTURES || state.caps[group].rm_so < 0)
      return 0;
    size_t length = (size_t)(state.caps[group].rm_eo - state.caps[group].rm_so);
    const char *captured = start + state.caps[group].rm_so;
    if ((size_t)(end - state.pos) < length)
      return 0;
    for (size_t i = 0; i < length; i++) {
      int a = (unsigned char)captured[i];
      int b = (unsigned char)state.pos[i];
      if (cflags & REG_ICASE) a = tolower(a), b = tolower(b);
      if (a != b)
        return 0;
    }
    state.pos += length;
    out[0] = state;
    return 1;
  }
  case RX_SEQ: {
    struct rx_state middle[RX_MAX_STATES];
    int n = rx_eval(node->left, start, end, cflags, eflags, in, middle);
    for (int i = 0; i < n; i++) {
      struct rx_state tail[RX_MAX_STATES];
      int m = rx_eval(node->right, start, end, cflags, eflags, &middle[i], tail);
      for (int j = 0; j < m; j++)
        rx_push(out, &count, &tail[j]);
    }
    return count;
  }
  case RX_ALT: {
    count = rx_eval(node->left, start, end, cflags, eflags, in, out);
    struct rx_state right[RX_MAX_STATES];
    int n = rx_eval(node->right, start, end, cflags, eflags, in, right);
    for (int i = 0; i < n; i++)
      rx_push(out, &count, &right[i]);
    return count;
  }
  case RX_REPEAT:
    return rx_eval_repeat(node, start, end, cflags, eflags, in, out);
  case RX_GROUP: {
    int group = node->group;
    if (group < RX_MAX_CAPTURES)
      state.caps[group].rm_so = (regoff_t)(state.pos - start);
    struct rx_state matches[RX_MAX_STATES];
    int n = rx_eval(node->left, start, end, cflags, eflags, &state, matches);
    for (int i = 0; i < n; i++) {
      if (group < RX_MAX_CAPTURES)
        matches[i].caps[group].rm_eo = (regoff_t)(matches[i].pos - start);
      rx_push(out, &count, &matches[i]);
    }
    return count;
  }
  }
  return 0;
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
  if (!preg || !pattern)
    return REG_BADPAT;
  memset(preg, 0, sizeof(*preg));
  struct rx_parser ps = {pattern, !!(cflags & REG_EXTENDED), 0, 0};
  struct rx_node *root = rx_parse_alt(&ps);
  if (!root || ps.error || *ps.p) {
    rx_free(root);
    return ps.error ? ps.error : REG_BADPAT;
  }
  preg->re_nsub = (size_t)ps.groups;
  preg->__root = root;
  preg->__cflags = cflags;
  return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
  if (!preg || !preg->__root || !string)
    return REG_NOMATCH;
  const char *end = string + strlen(string);
  for (const char *candidate = string; candidate <= end; candidate++) {
    struct rx_state initial;
    initial.pos = candidate;
    for (int i = 0; i < RX_MAX_CAPTURES; i++)
      initial.caps[i].rm_so = initial.caps[i].rm_eo = -1;
    struct rx_state matches[RX_MAX_STATES];
    int count = rx_eval(preg->__root, string, end, preg->__cflags, eflags,
                        &initial, matches);
    if (count) {
      int best = 0;
      for (int i = 1; i < count; i++)
        if (matches[i].pos > matches[best].pos)
          best = i;
      matches[best].caps[0].rm_so = (regoff_t)(candidate - string);
      matches[best].caps[0].rm_eo = (regoff_t)(matches[best].pos - string);
      if (!(preg->__cflags & REG_NOSUB) && pmatch) {
        size_t limit = nmatch < RX_MAX_CAPTURES ? nmatch : RX_MAX_CAPTURES;
        for (size_t i = 0; i < limit; i++)
          pmatch[i] = matches[best].caps[i];
        for (size_t i = limit; i < nmatch; i++)
          pmatch[i].rm_so = pmatch[i].rm_eo = -1;
      }
      return 0;
    }
    if (candidate < end && (preg->__cflags & REG_NEWLINE) == 0 &&
        ((struct rx_node *)preg->__root)->type == RX_BOL)
      break;
  }
  return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf,
                size_t errbuf_size) {
  (void)preg;
  const char *message = errcode == REG_NOMATCH ? "No match"
                        : errcode == REG_ESPACE ? "Out of memory"
                                               : "Invalid regular expression";
  size_t needed = strlen(message) + 1;
  if (errbuf && errbuf_size) {
    size_t copy = needed < errbuf_size ? needed : errbuf_size;
    memcpy(errbuf, message, copy - 1);
    errbuf[copy - 1] = '\0';
  }
  return needed;
}

void regfree(regex_t *preg) {
  if (!preg)
    return;
  rx_free((struct rx_node *)preg->__root);
  preg->__root = NULL;
  preg->re_nsub = 0;
}
