/* Two controllers use the standard atomic-wait value-check/park contract.
 * The mutex abstracts that atomic boundary, not a particular stdlib backend.
 * Finite final gate transition only; continuing rejected submissions/ABA are
 * outside this bounded model and have no bounded-return API guarantee.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
static atomic_int gate, stop, parked[2];
static pthread_mutex_t drain_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static void* controller(void* arg) {
    unsigned i = (unsigned)(uintptr_t)arg;
    pthread_mutex_lock(&drain_lock);
    atomic_store(&stop, 1);
    pthread_mutex_unlock(&drain_lock);
    int active = atomic_load(&gate);
    if (active) {
        pthread_mutex_lock(&wait_lock);
        if (atomic_load(&gate) == active) atomic_store(&parked[i], 1);
        pthread_mutex_unlock(&wait_lock);
    }
    return 0; /* Suspended continuation, if parked, resumes after notification. */
}
static void* submitter(void* unused) {
    (void)unused;
    int old = atomic_fetch_sub(&gate, 1);
#ifndef OMIT_CONTROLLER_NOTIFY
    if (old == 1 && atomic_load(&stop)) {
        pthread_mutex_lock(&wait_lock);
        atomic_store(&parked[0], 0);
        atomic_store(&parked[1], 0);
        pthread_mutex_unlock(&wait_lock);
    }
#endif
    return 0;
}
int main(void) {
    atomic_init(&gate, 1);
    pthread_t a, b, s;
    assert(pthread_create(&a, 0, controller, (void*)(uintptr_t)0) == 0);
    assert(pthread_create(&b, 0, controller, (void*)(uintptr_t)1) == 0);
    assert(pthread_create(&s, 0, submitter, 0) == 0);
    assert(pthread_join(a, 0) == 0);
    assert(pthread_join(b, 0) == 0);
    assert(pthread_join(s, 0) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        /* EVENTCOUNT_EXPECTED_ASSERTION */
        assert(atomic_load(&parked[i]) == 0);
    }
    return 0;
}
