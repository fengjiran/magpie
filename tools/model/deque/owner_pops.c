/* R1: fixed interleaving; legacy half-batch CAS produces a duplicate id. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
static atomic_int top,bottom,ready,owner_done;
static atomic_int slots[8];
static int popped[5],stolen[4];
static void* owner(void* unused) {
    (void)unused;
    while(!atomic_load_explicit(&ready,memory_order_acquire)){}
    for(unsigned i=0;i<5;++i) {
        int b=atomic_load_explicit(&bottom,memory_order_relaxed)-1;
        atomic_store_explicit(&bottom,b,memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        popped[i]=atomic_load_explicit(&slots[b],memory_order_relaxed);
    }
    atomic_store_explicit(&owner_done,1,memory_order_release);
    return NULL;
}
static void* thief(void* unused) {
    (void)unused;
    int t=atomic_load_explicit(&top,memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    int b=atomic_load_explicit(&bottom,memory_order_acquire);
    assert(t==0&&b==8);
#ifdef OLD_BATCH
    for(unsigned i=0;i<4;++i) stolen[i]=atomic_load_explicit(&slots[i],memory_order_relaxed);
    int amount=4;
#else
    stolen[0]=atomic_load_explicit(&slots[0],memory_order_relaxed);
    int amount=1;
#endif
    atomic_store_explicit(&ready,1,memory_order_release);
    while(!atomic_load_explicit(&owner_done,memory_order_acquire)){}
    assert(atomic_compare_exchange_strong_explicit(&top,&t,t+amount,memory_order_seq_cst,memory_order_seq_cst));
    return NULL;
}
int main(void) {
    for(unsigned i=0;i<8;++i) atomic_store_explicit(&slots[i],(int)i+1,memory_order_relaxed);
    atomic_thread_fence(memory_order_release);atomic_store_explicit(&bottom,8,memory_order_release);
    pthread_t a,b;assert(pthread_create(&a,NULL,owner,NULL)==0);assert(pthread_create(&b,NULL,thief,NULL)==0);
    assert(pthread_join(a,NULL)==0);assert(pthread_join(b,NULL)==0);
    unsigned seen[9]={0};for(unsigned i=0;i<5;++i) ++seen[popped[i]];
    for(unsigned i=0;i<4;++i) if(stolen[i]) ++seen[stolen[i]];
    for(unsigned i=1;i<=8;++i) {
        /* DEQUE_EXPECTED_ASSERTION */
        assert(seen[i]<=1);
    }
    return 0;
}
