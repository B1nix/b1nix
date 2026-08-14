/*
 * __cxa_thread_atexit_impl — thread-local static destructor registration.
 *
 * libc++abi's cxa_thread_atexit.cpp calls __cxa_thread_atexit_impl when the C
 * library provides it (glibc does). musl 1.2.5 deliberately does NOT — it is a
 * glibc-internal symbol — so a libc++/libc++abi shared object built with the
 * symbol assumed present is left with an unresolvable UND __cxa_thread_atexit_impl
 * and fails to load ("Error relocating: symbol not found"), killing every
 * libc++ consumer.
 *
 * This shim provides a correct implementation over musl's pthread TSD: each
 * registered destructor is pushed onto a per-thread list, and the list is run in
 * reverse (LIFO, as the C++ standard requires for thread_local objects) by the
 * TSD destructor when the thread exits. It is folded into libc++abi.so.1 at link
 * time (see build-libcxx-shared.sh), so it lives next to its only callers.
 */
#include <pthread.h>
#include <stdlib.h>

struct tls_dtor {
    void (*func)(void *);
    void *obj;
    struct tls_dtor *next;
};

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* TSD destructor: musl runs this at thread exit with the stored list head.
 * Walk the LIFO list, freeing each node after invoking its destructor. */
static void run_thread_dtors(void *head) {
    struct tls_dtor *d = head;
    while (d) {
        struct tls_dtor *next = d->next;
        d->func(d->obj);
        free(d);
        d = next;
    }
}

static void make_key(void) {
    pthread_key_create(&g_key, run_thread_dtors);
}

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle) {
    (void)dso_handle;
    pthread_once(&g_once, make_key);
    struct tls_dtor *d = malloc(sizeof(*d));
    if (!d)
        return -1;
    d->func = func;
    d->obj = obj;
    d->next = (struct tls_dtor *)pthread_getspecific(g_key);
    pthread_setspecific(g_key, d);
    return 0;
}
