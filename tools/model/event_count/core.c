/* Finite SC EventCount protocol in RC11. First wait can return spuriously;
 * the final wait records a suspended continuation, resumed after all notifiers.
 * kernel_lock models atomic futex compare/park versus wake, not an OS syscall.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef WORKERS
#define WORKERS 2
#endif
#ifndef ROUNDS
#define ROUNDS 1
#endif
static atomic_int epoch, waiters, stop, cursor;
static atomic_int word[3], parked[3];
static int registered[3];
static pthread_mutex_t kernel_lock[3] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static void enter(unsigned i) {
    atomic_fetch_add(&waiters, 1);
    atomic_store(&word[i], 1);
    registered[i] = 1;
}
static void leave(unsigned i) {
    atomic_exchange(&word[i], 0);
    atomic_fetch_sub(&waiters, 1);
    registered[i] = 0;
}
static int signal_one(unsigned i) {
#ifdef GENERIC
    pthread_mutex_lock(&kernel_lock[i]);
#endif
    int expected = 1;
    int signaled = atomic_compare_exchange_strong(&word[i], &expected, 2);
#ifndef GENERIC
    pthread_mutex_lock(&kernel_lock[i]);
#endif
    if (signaled) atomic_store(&parked[i], 0);
    pthread_mutex_unlock(&kernel_lock[i]);
    return signaled;
}
static void notify(int all) {
    atomic_fetch_add(&epoch, 1);
    if (!all && atomic_load(&waiters) == 0) return;
    unsigned start = (unsigned)atomic_fetch_add_explicit(&cursor, 1, memory_order_relaxed) % WORKERS;
    for (unsigned n=0;n<WORKERS;++n) {
        if (signal_one((start+n)%WORKERS) && !all) return;
    }
}
static void* worker(void* arg) {
    unsigned i=(unsigned)(uintptr_t)arg;
    for (unsigned round=0;round<ROUNDS;++round) {
        if (atomic_load(&stop)) return 0; /* Pre-registration hint alone is insufficient. */
        int key=atomic_load(&epoch);
        enter(i);
#ifndef OMIT_STOP_RECHECK
        if (atomic_load(&stop)) {leave(i);continue;}
#endif
        /* Queue probing is empty; an actual queue has its own acquire protocol. */
        if (atomic_load(&epoch)!=key) {leave(i);continue;}
        pthread_mutex_lock(&kernel_lock[i]);
        int suspend=round==ROUNDS-1 && atomic_load(&word[i])==1;
        if (suspend) atomic_store(&parked[i],1);
        pthread_mutex_unlock(&kernel_lock[i]);
        if (suspend) return 0; /* Retain registration until simulated resume. */
        leave(i); /* First round includes a legal spurious backend return. */
    }
    return 0;
}
static void* publisher(void* arg) {(void)arg;notify(0);return 0;}
static void* shutdown_thread(void* arg) {(void)arg;atomic_store(&stop,1);notify(1);return 0;}
int main(void) {
    pthread_t a,b,c,n,s;
    assert(pthread_create(&a,0,worker,(void*)(uintptr_t)0)==0);
#if WORKERS >= 2
    assert(pthread_create(&b,0,worker,(void*)(uintptr_t)1)==0);
#endif
#ifndef SINGLE_NOTIFIER
    assert(pthread_create(&n,0,publisher,0)==0);
#endif
#if WORKERS >= 3
    assert(pthread_create(&c,0,worker,(void*)(uintptr_t)2)==0);
#endif
    assert(pthread_create(&s,0,shutdown_thread,0)==0);
    assert(pthread_join(a,0)==0);
#if WORKERS >= 2
    assert(pthread_join(b,0)==0);
#endif
#ifndef SINGLE_NOTIFIER
    assert(pthread_join(n,0)==0);
#endif
#if WORKERS >= 3
    assert(pthread_join(c,0)==0);
#endif
    assert(pthread_join(s,0)==0);
    for(unsigned i=0;i<WORKERS;++i) {
        /* EVENTCOUNT_EXPECTED_ASSERTION */
        assert(atomic_load(&parked[i])==0);
        if(registered[i])leave(i);
    }
    assert(atomic_load(&waiters)==0);
    return 0;
}
