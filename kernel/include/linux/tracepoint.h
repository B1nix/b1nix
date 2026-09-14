/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TRACEPOINT_H
#define LKPI_LINUX_TRACEPOINT_H

/*
 * Tracepoint definition, with no tracing behind it.
 *
 * Two different things use this header and they need different treatment:
 *
 *   - Filesystems that keep their tracepoints in include/trace/events/. Those
 *     headers are not staged, and tools/fs/gen-shim-headers.sh generates a
 *     no-op macro per tracepoint from the pinned source instead.
 *   - Filesystems that keep them in their OWN directory — fs/iomap/trace.h is
 *     one — which IS staged and is therefore compiled. That file contains real
 *     TRACE_EVENT/DECLARE_EVENT_CLASS declarations, so the machinery has to
 *     exist and expand to nothing.
 *
 * The generator's second pass still exists, and covers a third case: names
 * built by token pasting inside a filesystem's own macro, which appear in no
 * text and in no TRACE_EVENT this header can see. It compiles every file and
 * turns whatever the compiler reports as an undeclared `trace_*` into a no-op
 * macro — the compiler being the only authority on what the macros expand to.
 */

#define TP_PROTO(args...)     args
#define TP_ARGS(args...)      args
#define TP_CONDITION(args...) args
#define TP_STRUCT__entry(args...)
#define TP_fast_assign(args...)
#define TP_printk(fmt, args...)
#define TP_perf_assign(args...)

#define __field(type, item)
#define __field_struct(type, item)
#define __field_desc(type, container, item)
#define __array(type, item, len)
#define __dynamic_array(type, item, len)
#define __string(item, src)
#define __string_len(item, src, len)
#define __assign_str(dst, src)
#define __get_str(field) ""
#define __get_dynamic_array(field) NULL
#define __get_dynamic_array_len(field) 0
#define __entry ((void *)0)
#define __print_symbolic(value, symbol_array...) ""
#define __print_flags(flag, delim, flag_array...) ""
#define __print_array(array, count, el_size) ""

/*
 * A tracepoint becomes an empty function with the right signature, not nothing
 * at all: call sites are ordinary calls, so they need something to call, and
 * the arguments are evaluated — which matches Linux with tracing compiled in
 * and disabled, so a call site with a side effect behaves the same either way.
 *
 * `DECLARE_EVENT_CLASS` defines no function: it is a template, and the
 * `DEFINE_EVENT` that instantiates it carries the same prototype.
 */
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) \
	static inline void trace_##name(proto) { }
#define TRACE_EVENT_FN(name, proto, args, tstruct, assign, print, reg, unreg) \
	static inline void trace_##name(proto) { }
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) \
	static inline void trace_##name(proto) { }
#define TRACE_EVENT_FLAGS(name, value)
#define TRACE_EVENT_PERF_PERM(name, expr...)
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args) \
	static inline void trace_##name(proto) { }
#define DEFINE_EVENT_FN(template, name, proto, args, reg, unreg) \
	static inline void trace_##name(proto) { }
#define DEFINE_EVENT_PRINT(template, name, proto, args, print) \
	static inline void trace_##name(proto) { }
#define DEFINE_EVENT_CONDITION(template, name, proto, args, cond) \
	static inline void trace_##name(proto) { }
#define DECLARE_TRACE(name, proto, args) \
	static inline void trace_##name(proto) { }
#define DEFINE_TRACE(name) struct lkpi_trace_##name##_unused
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name) struct lkpi_tracesym_##name##_unused
#define EXPORT_TRACEPOINT_SYMBOL(name)     struct lkpi_tracesym_##name##_unused
#define TRACE_DEFINE_ENUM(x)
#define TRACE_DEFINE_SIZEOF(x)

#endif
