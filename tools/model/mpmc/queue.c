/* RC11 projection of MPMCQueue<T>: ordinary data, relaxed position CAS,
 * acquire/release seq. Bounded tickets exercise capacity-2 reuse, not overflow.
 * C++ payload traits, allocation and fail-fast are tested in the native header.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

struct slot { atomic_size_t seq; int data; };
static struct slot slots[2];
static atomic_size_t tail, head;
static int accepted[3];
static int observed[4];
static size_t bases[2] = {0,2};

static int enqueue(int value) {
    size_t pos = atomic_load_explicit(&tail, memory_order_relaxed);
    struct slot* slot;
    for (;;) {
        slot = &slots[pos & 1];
        size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        if (seq == pos) {
            if (atomic_compare_exchange_weak_explicit(&tail, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) break;
        } else if (seq < pos) { return 0; }
        else { pos = atomic_load_explicit(&tail, memory_order_relaxed); }
    }
#ifdef BROKEN_PUBLICATION
    /* MPMC_EXPECTED_RACE: publish the token before the ordinary payload. */
    atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
    slot->data = value;
#else
    slot->data = value;
    atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
#endif
    return 1;
}
static int dequeue(int* value) {
    size_t pos = atomic_load_explicit(&head, memory_order_relaxed);
    struct slot* slot;
    for (;;) {
        slot = &slots[pos & 1];
        size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        if (seq == pos + 1) {
            if (atomic_compare_exchange_weak_explicit(&head, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) break;
        } else if (seq < pos + 1) { return 0; }
        else { pos = atomic_load_explicit(&head, memory_order_relaxed); }
    }
    *value = slot->data;
    atomic_store_explicit(&slot->seq, pos + 2, memory_order_release);
    return 1;
}
static void* producer0(void* unused) {
    (void)unused;
    accepted[0] = enqueue(1);
    accepted[2] = enqueue(3);
    return NULL;
}
static void* producer1(void* unused) {
    (void)unused;
    accepted[1] = enqueue(2);
    return NULL;
}
static void* consumer(void* unused) {
    size_t base = *(size_t*)unused;
    (void)dequeue(&observed[base]);
    (void)dequeue(&observed[base + 1]);
    return NULL;
}
int main(void) {
    atomic_init(&slots[0].seq, 0);
    atomic_init(&slots[1].seq, 1);
    pthread_t a,b,c;
    assert(pthread_create(&a,NULL,producer0,NULL)==0);
    assert(pthread_create(&b,NULL,producer1,NULL)==0);
    assert(pthread_create(&c,NULL,consumer,&bases[0])==0);
#ifdef TWO_CONSUMERS
    pthread_t d;
    assert(pthread_create(&d,NULL,consumer,&bases[1])==0);
#endif
    assert(pthread_join(a,NULL)==0);
    assert(pthread_join(b,NULL)==0);
    assert(pthread_join(c,NULL)==0);
#ifdef TWO_CONSUMERS
    assert(pthread_join(d,NULL)==0);
#endif
    unsigned seen[4] = {0};
    for (unsigned i=0;i<4;++i) {
        assert(observed[i]>=0 && observed[i]<=3);
        if (observed[i]) ++seen[observed[i]];
    }
    int value=0;
    while (dequeue(&value)) {
        assert(value>=1 && value<=3);
        ++seen[value];
    }
    for (unsigned i=0;i<3;++i) { assert(seen[i+1] == (unsigned)accepted[i]); }
    return 0;
}
