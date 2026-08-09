/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_TRACEPOINT_H
#define LKPI_LINUX_TRACEPOINT_H
/* Static tracepoints. b1nix has ftrace for its own functions but no tracepoint
 * registry the imported ones could attach to, so they compile away. */
/*
 * A tracepoint becomes an empty function with the right signature, not nothing
 * at all: call sites are ordinary calls, so they need something to call. The
 * arguments are evaluated — which matches Linux with tracing compiled in and
 * disabled, and means a call site with a side effect behaves the same either
 * way.
 */
#define TP_PROTO(args...) args
#define TP_ARGS(args...)  args
#define TP_STRUCT__entry(args...)
#define TP_fast_assign(args...)
#define TP_printk(fmt, args...)

#define DECLARE_TRACE(name, proto, args) \
	static inline void trace_##name(proto) { }
#define DEFINE_TRACE(name)               struct lkpi_trace_##name##_unused
#define EXPORT_TRACEPOINT_SYMBOL(name)   struct lkpi_tracesym_##name##_unused
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) \
	static inline void trace_##name(proto) { }
#define DEFINE_EVENT(tmpl, name, proto, args) \
	static inline void trace_##name(proto) { }
#endif
