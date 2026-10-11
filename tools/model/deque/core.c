/* Bounded RC11 projection of the production signed indices/atomic slots.
 * IDs stand for Task*; payloads are ordinary data read only after ownership.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
static atomic_llong top,bottom;
static atomic_int slots[2];
static int payload[4], accepted[4], owner_result[2], thief_result[2], extra_result;
static int push(int id) {
    long long b=atomic_load_explicit(&bottom,memory_order_relaxed);
    long long t=atomic_load_explicit(&top,memory_order_acquire);
    if(b-t>=2) return 0;
    atomic_store_explicit(&slots[(size_t)b&1],id,memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&bottom,b+1,memory_order_release);
    return 1;
}
static int pop(void) {
    long long b=atomic_load_explicit(&bottom,memory_order_relaxed)-1;
    atomic_store_explicit(&bottom,b,memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    long long t=atomic_load_explicit(&top,memory_order_relaxed);
    int x=0;
    if(t<=b) {
        x=atomic_load_explicit(&slots[(size_t)b&1],memory_order_relaxed);
        if(t==b) {
            if(!atomic_compare_exchange_strong_explicit(&top,&t,t+1,memory_order_seq_cst,memory_order_seq_cst)) x=0;
#ifdef BAD_RESTORE
            atomic_store_explicit(&bottom,t+1,memory_order_relaxed);
#else
            atomic_store_explicit(&bottom,b+1,memory_order_relaxed);
#endif
        }
    } else atomic_store_explicit(&bottom,b+1,memory_order_relaxed);
    return x;
}
static int steal(void) {
    long long t=atomic_load_explicit(&top,memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    long long b=atomic_load_explicit(&bottom,memory_order_acquire);
    if(t>=b) return 0;
    int x=atomic_load_explicit(&slots[(size_t)t&1],memory_order_relaxed);
    if(!atomic_compare_exchange_strong_explicit(&top,&t,t+1,memory_order_seq_cst,memory_order_seq_cst)) return 0;
    return x;
}
static void check_payload(int id) {if(id) {assert(id>=1&&id<=3);assert(payload[id]==id*100);}}
static void* owner(void* unused) {
    (void)unused;
#ifndef SINGLE
    payload[1]=100;accepted[1]=push(1);
    payload[2]=200;accepted[2]=push(2);
    owner_result[0]=pop();check_payload(owner_result[0]);
    payload[3]=300;accepted[3]=push(3);
    owner_result[1]=pop();check_payload(owner_result[1]);
#else
    owner_result[0]=pop();check_payload(owner_result[0]);
#endif
    return NULL;
}
static void* thief(void* unused) {
    (void)unused;
    thief_result[0]=steal();check_payload(thief_result[0]);
#ifndef SINGLE
    thief_result[1]=steal();check_payload(thief_result[1]);
#endif
    return NULL;
}
#ifdef TWO_THIEVES
static void* extra_thief(void* unused) {
    (void)unused;
    extra_result=steal();check_payload(extra_result);
    return NULL;
}
#endif
int main(void) {
#ifdef SINGLE
    payload[1]=100;accepted[1]=push(1);
#endif
    pthread_t a,b;
    assert(pthread_create(&a,NULL,owner,NULL)==0);
    assert(pthread_create(&b,NULL,thief,NULL)==0);
#ifdef TWO_THIEVES
    pthread_t c; assert(pthread_create(&c,NULL,extra_thief,NULL)==0);
    assert(pthread_join(c,NULL)==0);
#endif
    assert(pthread_join(a,NULL)==0);assert(pthread_join(b,NULL)==0);
    unsigned seen[4]={0};
    if(extra_result) ++seen[extra_result];
    for(unsigned i=0;i<2;++i) {
        if(owner_result[i]) ++seen[owner_result[i]];
        if(thief_result[i]) ++seen[thief_result[i]];
    }
#ifdef SINGLE
    /* DEQUE_EXPECTED_ASSERTION */
    assert(atomic_load_explicit(&top,memory_order_relaxed)==atomic_load_explicit(&bottom,memory_order_relaxed));
#else
    int id;
    while((id=pop())) {check_payload(id);++seen[id];}
#endif
    for(unsigned i=1;i<=3;++i) assert(seen[i]==(unsigned)accepted[i]);
    return 0;
}
