#ifndef B1NIX_U_ASSERT_H
#define B1NIX_U_ASSERT_H

#ifdef NDEBUG
#define assert(ignore) ((void)0)
#else
#define assert(expr) \
    do { \
        if (!(expr)) { \
            /* Add minimal assert logging if needed */ \
            while (1); \
        } \
    } while (0)
#endif

#endif
