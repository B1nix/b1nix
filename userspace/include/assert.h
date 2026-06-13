#undef assert

#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
#ifdef __cplusplus
extern "C" {
#endif
void _exit(int status) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
#define assert(expr) ((expr) ? (void)0 : _exit(139))
#endif

/* C11: static_assert is a convenience macro for _Static_assert. In C++ it is a
 * keyword already (and libstdc++ uses it), so only define the macro for C. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
