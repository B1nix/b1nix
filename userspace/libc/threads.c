/*
 * threads.c — C11 threads implementation over pthreads for b1nix.
 */
#include <threads.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <errno.h>

int thrd_create(thrd_t *t, thrd_start_t fn, void *arg) {
    return pthread_create(t, NULL, (void *(*)(void *))fn, arg) == 0 ? thrd_success : thrd_error;
}
thrd_t thrd_current(void) { return pthread_self(); }
int thrd_detach(thrd_t t) { return pthread_detach(t) == 0 ? thrd_success : thrd_error; }
int thrd_equal(thrd_t a, thrd_t b) { return pthread_equal(a, b); }
void thrd_exit(int res) { pthread_exit((void *)(long)res); }
int thrd_join(thrd_t t, int *res) {
    void *val;
    if (pthread_join(t, &val) != 0) return thrd_error;
    if (res) *res = (int)(long)val;
    return thrd_success;
}
int thrd_sleep(const struct timespec *dur, struct timespec *rem) {
    return nanosleep(dur, rem) == 0 ? 0 : -1;
}
void thrd_yield(void) { sched_yield(); }

int mtx_init(mtx_t *m, int type) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (type & 1) /* mtx_recursive */
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return r == 0 ? thrd_success : thrd_error;
}
void mtx_destroy(mtx_t *m) { pthread_mutex_destroy(m); }
int mtx_lock(mtx_t *m) { return pthread_mutex_lock(m) == 0 ? thrd_success : thrd_error; }
int mtx_trylock(mtx_t *m) { return pthread_mutex_trylock(m) == 0 ? thrd_success : thrd_busy; }
int mtx_unlock(mtx_t *m) { return pthread_mutex_unlock(m) == 0 ? thrd_success : thrd_error; }

int cnd_init(cnd_t *c) { return pthread_cond_init(c, NULL) == 0 ? thrd_success : thrd_error; }
void cnd_destroy(cnd_t *c) { pthread_cond_destroy(c); }
int cnd_signal(cnd_t *c) { return pthread_cond_signal(c) == 0 ? thrd_success : thrd_error; }
int cnd_broadcast(cnd_t *c) { return pthread_cond_broadcast(c) == 0 ? thrd_success : thrd_error; }
int cnd_wait(cnd_t *c, mtx_t *m) { return pthread_cond_wait(c, m) == 0 ? thrd_success : thrd_error; }
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *ts) {
    int r = pthread_cond_timedwait(c, m, ts);
    if (r == 0) return thrd_success;
    if (r == ETIMEDOUT) return thrd_timedout;
    return thrd_error;
}

void call_once(once_flag *flag, void (*fn)(void)) {
    pthread_once(&flag->__once, fn);
}

int tss_create(tss_t *key, tss_dtor_t dtor) {
    return pthread_key_create(key, dtor) == 0 ? thrd_success : thrd_error;
}
void tss_delete(tss_t key) { pthread_key_delete(key); }
void *tss_get(tss_t key) { return pthread_getspecific(key); }
int tss_set(tss_t key, void *val) {
    return pthread_setspecific(key, val) == 0 ? thrd_success : thrd_error;
}
