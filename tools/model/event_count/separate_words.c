/* R5: A passed epoch recheck, B prepared an old key, notify completed,
 * then B enters. Only the historical shared-generation variant can sleep. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
static atomic_int epoch, words[2], shared_word;
static atomic_int a_ready,b_ready,notified,b_entered;
static void* a(void* arg) {
    (void)arg;
    int key=atomic_load(&epoch);
#ifdef SHARED_GENERATION
    atomic_store(&shared_word,key);
#else
    atomic_store(&words[0],1);
#endif
    assert(atomic_load(&epoch)==key);
    atomic_store(&a_ready,1);
    while(!atomic_load(&b_entered)){}
#ifdef SHARED_GENERATION
    int may_sleep=atomic_load(&shared_word)==key;
#else
    int may_sleep=atomic_load(&words[0])==1;
#endif
    /* EVENTCOUNT_EXPECTED_ASSERTION */
    assert(!may_sleep);
    return 0;
}
static void* b(void* arg) {
    (void)arg;int old_key=atomic_load(&epoch);atomic_store(&b_ready,1);
    while(!atomic_load(&notified)){}
#ifdef SHARED_GENERATION
    atomic_store(&shared_word,old_key);
#else
    (void)old_key;atomic_store(&words[1],1);
#endif
    atomic_store(&b_entered,1);return 0;
}
int main(void) {
    pthread_t x,y;assert(pthread_create(&x,0,a,0)==0);assert(pthread_create(&y,0,b,0)==0);
    while(!atomic_load(&a_ready)||!atomic_load(&b_ready)){}
    atomic_fetch_add(&epoch,1);
#ifdef SHARED_GENERATION
    atomic_store(&shared_word,1);
#else
    int waiting=1;assert(atomic_compare_exchange_strong(&words[0],&waiting,2));
#endif
    atomic_store(&notified,1);
    assert(pthread_join(x,0)==0);assert(pthread_join(y,0)==0);return 0;
}
