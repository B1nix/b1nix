/*
 * /bin/js — standalone JavaScript interpreter for b1nix (M58).
 *
 * Embeds the Duktape 2.7.0 ECMAScript engine (the same engine NetSurf vendors,
 * M54) as a tiny self-contained REPL / script runner. Duktape is a single-file
 * ANSI C amalgamation that only needs malloc/free/printf/setjmp and a handful of
 * libm functions — all provided by libb1nix + the ported openlibm.
 *
 * Usage:
 *   js               run a line-at-a-time REPL over stdin
 *   js FILE          read FILE, evaluate it as a script
 *   js -e "CODE"     evaluate CODE directly
 *
 * A minimal print()/console.log() binding writes its arguments to stdout so JS
 * programs can produce output. JavaScript errors are caught (duk_peval*) and
 * reported instead of aborting the process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duktape.h"

/* ---- print()/console.log() native binding ---------------------------------
 * Joins all arguments with a single space (coercing each to string) and writes
 * the result followed by a newline to stdout. */
static duk_ret_t native_print(duk_context *ctx)
{
	duk_idx_t n = duk_get_top(ctx);
	duk_idx_t i;

	for (i = 0; i < n; i++) {
		if (i > 0)
			fputc(' ', stdout);
		fputs(duk_safe_to_string(ctx, i), stdout);
	}
	fputc('\n', stdout);
	fflush(stdout);
	return 0; /* no return value */
}

/* Install print() as a global, and console.log/console.error aliases. */
static void register_bindings(duk_context *ctx)
{
	/* global print() */
	duk_push_c_function(ctx, native_print, DUK_VARARGS);
	duk_put_global_string(ctx, "print");

	/* global console = { log: print, error: print } */
	duk_push_global_object(ctx);
	duk_push_object(ctx); /* console */
	duk_push_c_function(ctx, native_print, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "log");
	duk_push_c_function(ctx, native_print, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "error");
	duk_put_prop_string(ctx, -2, "console");
	duk_pop(ctx); /* pop global object */
}

/* Read an entire file into a malloc'd, NUL-terminated buffer. */
static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	size_t cap = 4096, len = 0;
	int c;

	if (!f)
		return NULL;

	buf = malloc(cap);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	while ((c = fgetc(f)) != -1) {
		if (len + 1 >= cap) {
			char *nb;
			cap *= 2;
			nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
		}
		buf[len++] = (char)c;
	}
	buf[len] = '\0';
	fclose(f);
	if (out_len)
		*out_len = len;
	return buf;
}

/* Evaluate a string of JavaScript. Prints the result (or the error). Returns 0
 * on success, non-zero on a JS error. Uses duk_peval_string so a thrown error
 * is converted to a return code instead of a longjmp out of our frame. */
static int eval_string(duk_context *ctx, const char *src, int print_result)
{
	int rc = duk_peval_string(ctx, src);

	if (rc != 0) {
		/* Error: the error object is on the stack top. */
		fprintf(stderr, "%s\n", duk_safe_to_string(ctx, -1));
		duk_pop(ctx);
		return 1;
	}

	if (print_result) {
		/* Print the result unless it is undefined (REPL convention). */
		if (!duk_is_undefined(ctx, -1)) {
			printf("= %s\n", duk_safe_to_string(ctx, -1));
			fflush(stdout);
		}
	}
	duk_pop(ctx); /* pop result */
	return 0;
}

/* Line-at-a-time REPL over stdin. */
static int run_repl(duk_context *ctx)
{
	char line[4096];

	printf("b1nix js (Duktape %ld.%ld.%ld) — Ctrl-D to exit\n",
	       (long)(DUK_VERSION / 10000),
	       (long)((DUK_VERSION / 100) % 100),
	       (long)(DUK_VERSION % 100));
	fflush(stdout);

	for (;;) {
		fputs("js> ", stdout);
		fflush(stdout);
		if (!fgets(line, (int)sizeof(line), stdin))
			break; /* EOF */
		/* Skip blank lines. */
		if (line[0] == '\n' || line[0] == '\0')
			continue;
		(void)eval_string(ctx, line, 1);
	}
	fputc('\n', stdout);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	duk_context *ctx;
	int rc = 0;

	ctx = duk_create_heap_default();
	if (!ctx) {
		fprintf(stderr, "js: failed to create Duktape heap\n");
		return 1;
	}
	register_bindings(ctx);

	if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
		/* js -e "CODE" — evaluate inline. */
		rc = eval_string(ctx, argv[2], 0);
	} else if (argc >= 2) {
		/* js FILE — run a script file. */
		size_t len;
		char *src = read_file(argv[1], &len);
		if (!src) {
			fprintf(stderr, "js: cannot read '%s'\n", argv[1]);
			duk_destroy_heap(ctx);
			return 1;
		}
		rc = eval_string(ctx, src, 0);
		free(src);
	} else {
		/* No args — interactive REPL. */
		rc = run_repl(ctx);
	}

	duk_destroy_heap(ctx);
	return rc;
}
