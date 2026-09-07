/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Upstream, including this at the bottom of a trace header is what causes the
 * header to be re-read with the macros redefined to emit the tracepoint
 * definitions. With no tracing there is nothing to emit and nothing to re-read.
 *
 * Deliberately without an include guard: it is included once per trace header
 * by design, and a guard would be a promise this file does not need to make.
 */
