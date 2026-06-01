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
