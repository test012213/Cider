#include "../mac.h"
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static long counter;
static int ready;
static __thread int tls_value = 100;
static int once_calls;
static void init_once(void) { once_calls++; }

static void *worker(void *arg)
{
    long id = (long)arg;
    pthread_once(&once, init_once);
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    tls_value += (int)id;
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)tls_value;
}

int main(void)
{
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, worker, (void *)i);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    long sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("counter=%ld tls_sum=%ld main_tls=%d once=%d\n", counter, sum, tls_value, once_calls);
    return counter == 400000 && sum == 406 && tls_value == 100 && once_calls == 1 ? 0 : 1;
}
