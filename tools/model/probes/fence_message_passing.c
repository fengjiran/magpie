#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_int payload = 0;
static atomic_int ready = 0;
static int observed_ready;
static int observed_payload;

static void* writer(void* unused) {
    (void)unused;
    atomic_store_explicit(&payload, 42, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&ready, 1, memory_order_relaxed);
    return NULL;
}

static void* reader(void* unused) {
    (void)unused;
    observed_ready = atomic_load_explicit(&ready, memory_order_relaxed);
    if (observed_ready == 1) {
        atomic_thread_fence(memory_order_acquire);
        observed_payload = atomic_load_explicit(&payload, memory_order_relaxed);
    }
    return NULL;
}

int main(void) {
    pthread_t producer;
    pthread_t consumer;
    assert(pthread_create(&producer, NULL, writer, NULL) == 0);
    assert(pthread_create(&consumer, NULL, reader, NULL) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(observed_ready == 0 || observed_payload == 42);
    return 0;
}
