/* Bounded projection of closing waits, the admission gate and final pending.
 * A suspended continuation is represented by parked; this is not a syscall
 * model or an arbitrary-size task/scheduler liveness proof.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef WORKERS
#define WORKERS 1
#endif
#ifndef ROUNDS
#define ROUNDS 1
#endif
static atomic_int epoch, waiters, stop, gate, pending;
static atomic_int word[2], parked[2];
static int registered[2];
static pthread_mutex_t drain_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kernel_lock[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

static int can_exit(void) {
    return atomic_load(&stop) && atomic_load(&gate) == 0 &&
        atomic_load_explicit(&pending, memory_order_acquire) == 0;
}
static void leave(unsigned i) {
    atomic_exchange(&word[i], 0);
    atomic_fetch_sub(&waiters, 1);
    registered[i] = 0;
}
static void notify_all(void) {
    atomic_fetch_add(&epoch, 1);
    for (unsigned i = 0; i < WORKERS; ++i) {
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
    }
}
static void* worker(void* arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        if (can_exit()) return 0;
        int key = atomic_load(&epoch);
        atomic_fetch_add(&waiters, 1);
        atomic_store(&word[i], 1);
        registered[i] = 1;
#ifndef OMIT_EXIT_RECHECK
        if (can_exit()) { leave(i); continue; }
#endif
        /* The final queue scan has no task. Pending can be an executing task,
         * and a rejected capture can hold the gate with pending == 0. */
        if (atomic_load(&epoch) != key) { leave(i); continue; }
        pthread_mutex_lock(&kernel_lock[i]);
        int suspend = round == ROUNDS - 1 && atomic_load(&word[i]) == 1;
        if (suspend) atomic_store(&parked[i], 1);
        pthread_mutex_unlock(&kernel_lock[i]);
        if (suspend) return 0;
        leave(i); /* A legal spurious return in the earlier round. */
    }
    return 0;
}
static void finish_pending(void) {
#ifndef GATE_ONLY
    int old = atomic_fetch_sub_explicit(&pending, 1, memory_order_acq_rel);
    if (old == 1) {
        pthread_mutex_lock(&drain_lock);
        int stopping = atomic_load(&stop);
        pthread_mutex_unlock(&drain_lock);
#ifndef OMIT_PENDING_NOTIFY
        if (stopping) notify_all();
#endif
    }
#endif
}
static void finish_gate(void) {
#ifndef PENDING_ONLY
    int old = atomic_fetch_sub(&gate, 1);
#ifndef OMIT_GATE_NOTIFY
    if (old == 1 && atomic_load(&stop)) notify_all();
#endif
#endif
}
static void* finish_thread(void* unused) {
    (void)unused;
#ifdef GATE_FIRST
    finish_gate(); finish_pending();
#else
    finish_pending(); finish_gate();
#endif
    return 0;
}
static void* shutdown_thread(void* unused) {
    (void)unused;
#ifndef OMIT_STOP_MUTEX
    pthread_mutex_lock(&drain_lock);
#endif
    atomic_store(&stop, 1);
#ifndef OMIT_STOP_MUTEX
    pthread_mutex_unlock(&drain_lock);
#endif
    notify_all();
    return 0;
}
int main(void) {
#ifdef ALREADY_STOPPED
    atomic_init(&stop, 1);
    atomic_init(&epoch, 1); /* Initial stop notification predates registration. */
#endif
#ifndef PENDING_ONLY
    atomic_init(&gate, 1);
#endif
#ifndef GATE_ONLY
    atomic_init(&pending, 1);
#endif
    pthread_t a, b, finish, shutdown;
    assert(pthread_create(&a, 0, worker, (void*)(uintptr_t)0) == 0);
#if WORKERS == 2
    assert(pthread_create(&b, 0, worker, (void*)(uintptr_t)1) == 0);
#endif
    assert(pthread_create(&finish, 0, finish_thread, 0) == 0);
#ifndef ALREADY_STOPPED
    assert(pthread_create(&shutdown, 0, shutdown_thread, 0) == 0);
#endif
    assert(pthread_join(a, 0) == 0);
#if WORKERS == 2
    assert(pthread_join(b, 0) == 0);
#endif
    assert(pthread_join(finish, 0) == 0);
#ifndef ALREADY_STOPPED
    assert(pthread_join(shutdown, 0) == 0);
#endif
    assert(can_exit());
    for (unsigned i = 0; i < WORKERS; ++i) {
        /* EVENTCOUNT_EXPECTED_ASSERTION */
        assert(atomic_load(&parked[i]) == 0);
        if (registered[i]) leave(i);
    }
    assert(atomic_load(&waiters) == 0);
    return 0;
}
