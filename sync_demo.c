/* Unit 2 lab-style demo: pthread_create + mutex + semaphore + condition variable. */
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

static int shared_value = 0;
static int ready = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static sem_t done;

static void *producer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    shared_value = 42;
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    sem_post(&done);
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    printf("[SYNC] consumer read shared_value=%d\n", shared_value);
    pthread_mutex_unlock(&lock);
    sem_post(&done);
    return NULL;
}

int main(void)
{
    pthread_t p, c;
    sem_init(&done, 0, 0);
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    sem_wait(&done); sem_wait(&done);
    pthread_join(p, NULL); pthread_join(c, NULL);
    sem_destroy(&done);
    pthread_mutex_destroy(&lock); pthread_cond_destroy(&cond);
    return 0;
}
