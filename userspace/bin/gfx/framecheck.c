/*
 * framecheck — did the compositor actually paint the colour it was told to?
 *
 * grim pulls a frame out of a running compositor through wlr-screencopy and
 * writes it here as a binary PPM. "grim exited 0" only says a buffer arrived;
 * it says nothing about what is in it, and a black or garbage frame exits 0
 * exactly the same way. So the picture is read back and its pixels are
 * compared against the colour the compositor was asked to paint.
 *
 * A grid of samples rather than one pixel: a single probe in the middle passes
 * on an image that is right in one place and wrong everywhere else, and it
 * also passes on a frame whose stride is wrong in a way that happens to line
 * up there. Sampling across the whole image catches both.
 *
 * Usage:  framecheck <file.ppm> <rrggbb> [tolerance]
 * Exit 0 when every sample matches within the tolerance, 1 otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PPM headers may carry comments between any two tokens. */
static int next_token(FILE *f, char *buf, size_t n) {
  size_t i = 0;
  int c;
  for (;;) {
    c = fgetc(f);
    if (c == EOF)
      return -1;
    if (c == '#') {
      while (c != EOF && c != '\n')
        c = fgetc(f);
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      continue;
    break;
  }
  while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
    if (i + 1 < n)
      buf[i++] = (char)c;
    c = fgetc(f);
  }
  buf[i] = '\0';
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: framecheck <file.ppm> <rrggbb> [tolerance]\n");
    return 2;
  }
  const char *path = argv[1];
  unsigned long want = strtoul(argv[2], NULL, 16);
  int tol = argc > 3 ? atoi(argv[3]) : 16;
  int wr = (int)((want >> 16) & 0xff);
  int wg = (int)((want >> 8) & 0xff);
  int wb = (int)(want & 0xff);

  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("FRAMECHECK: fail open %s\n", path);
    return 1;
  }

  char tok[64];
  if (next_token(f, tok, sizeof(tok)) != 0 || strcmp(tok, "P6") != 0) {
    printf("FRAMECHECK: fail not-a-binary-ppm (%s)\n", tok);
    fclose(f);
    return 1;
  }
  long w = 0, h = 0, maxval = 0;
  if (next_token(f, tok, sizeof(tok)) != 0)
    goto bad_header;
  w = strtol(tok, NULL, 10);
  if (next_token(f, tok, sizeof(tok)) != 0)
    goto bad_header;
  h = strtol(tok, NULL, 10);
  if (next_token(f, tok, sizeof(tok)) != 0)
    goto bad_header;
  maxval = strtol(tok, NULL, 10);
  if (w <= 0 || h <= 0 || maxval != 255) {
    printf("FRAMECHECK: fail header w=%ld h=%ld maxval=%ld\n", w, h, maxval);
    fclose(f);
    return 1;
  }

  size_t rowlen = (size_t)w * 3;
  unsigned char *row = malloc(rowlen);
  if (!row) {
    printf("FRAMECHECK: fail out-of-memory %zu\n", rowlen);
    fclose(f);
    return 1;
  }

  /* Up to 16 sample rows and 16 columns, spread evenly and kept off the very
   * edge, where a compositor's own border or a rounding error lives. */
  const int grid = 16;
  int sampled = 0, matched = 0;
  int first_bad_x = -1, first_bad_y = -1, bad_r = 0, bad_g = 0, bad_b = 0;

  for (long y = 0; y < h; y++) {
    if (fread(row, 1, rowlen, f) != rowlen) {
      printf("FRAMECHECK: fail short-read at row %ld of %ld\n", y, h);
      free(row);
      fclose(f);
      return 1;
    }
    int is_sample_row = 0;
    for (int j = 0; j < grid && !is_sample_row; j++)
      if ((h * (j * 2 + 1)) / (grid * 2) == y)
        is_sample_row = 1;
    if (!is_sample_row)
      continue;

    for (int i = 0; i < grid; i++) {
      long x = (w * (i * 2 + 1)) / (grid * 2);
      if (x >= w)
        x = w - 1;
      int r = row[x * 3 + 0], g = row[x * 3 + 1], b = row[x * 3 + 2];
      sampled++;
      if (abs(r - wr) <= tol && abs(g - wg) <= tol && abs(b - wb) <= tol) {
        matched++;
      } else if (first_bad_x < 0) {
        first_bad_x = (int)x;
        first_bad_y = (int)y;
        bad_r = r;
        bad_g = g;
        bad_b = b;
      }
    }
  }
  free(row);
  fclose(f);

  int ok = sampled > 0 && matched == sampled;
  printf("FRAMECHECK: %s %s %ldx%ld want=%d,%d,%d tol=%d sampled=%d "
         "matched=%d",
         ok ? "ok" : "fail", path, w, h, wr, wg, wb, tol, sampled, matched);
  if (!ok && first_bad_x >= 0)
    printf(" first-mismatch=(%d,%d)=%d,%d,%d", first_bad_x, first_bad_y, bad_r,
           bad_g, bad_b);
  printf("\n");
  return ok ? 0 : 1;

bad_header:
  printf("FRAMECHECK: fail truncated-header\n");
  fclose(f);
  return 1;
}
