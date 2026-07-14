/*
 * threads.h — C11 threads implementation over pthreads for b1nix.
 */
#ifndef _THREADS_H
#define _THREADS_H

#include <pthread.h>
#include <time.h>
#include <sched.h>

#define ONCE_FLAG_INIT { PTHREAD_ONCE_INIT }
#define TSS_DTOR_ITERATIONS 4

typedef struct { pthread_once_t __once; } once_flag;

enum { thrd_success = 0, thrd_nomem = 1, thrd_timedout = 2, thrd_busy = 3, thrd_error = -1 };

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_key_t tss_t;
typedef int (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);

/* Threads */
int thrd_create(thrd_t *t, thrd_start_t fn, void *arg);
thrd_t thrd_current(void);
int thrd_detach(thrd_t t);
int thrd_equal(thrd_t a, thrd_t b);
void thrd_exit(int res);
int thrd_join(thrd_t t, int *res);
int thrd_sleep(const struct timespec *dur, struct timespec *rem);
void thrd_yield(void);

/* Mutexes */
int mtx_init(mtx_t *m, int type);
void mtx_destroy(mtx_t *m);
int mtx_lock(mtx_t *m);
int mtx_trylock(mtx_t *m);
int mtx_unlock(mtx_t *m);

/* Condition variables */
int cnd_init(cnd_t *c);
void cnd_destroy(cnd_t *c);
int cnd_signal(cnd_t *c);
int cnd_broadcast(cnd_t *c);
int cnd_wait(cnd_t *c, mtx_t *m);
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *ts);

/* Once */
void call_once(once_flag *flag, void (*fn)(void));

/* Thread-specific storage */
int tss_create(tss_t *key, tss_dtor_t dtor);
void tss_delete(tss_t key);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *val);

#endif /* _THREADS_H */
