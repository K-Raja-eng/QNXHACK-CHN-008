/* Unit 3 lab-style demo: shm_open + mmap + process-shared mutex. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

#define NAME "/qnx_shm_demo"

typedef struct { pthread_mutex_t lock; int value; } Shared;

int main(void)
{
    int fd = shm_open(NAME, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) return 1;
    if (ftruncate(fd, sizeof(Shared)) != 0) return 1;
    Shared *s = mmap(NULL, sizeof(Shared), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) return 1;
    memset(s, 0, sizeof(*s));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutex_lock(&s->lock);
    s->value = 1234;
    printf("[SHM] value written to shared memory = %d\n", s->value);
    pthread_mutex_unlock(&s->lock);

    pthread_mutex_destroy(&s->lock);
    munmap(s, sizeof(*s)); close(fd); shm_unlink(NAME);
    return 0;
}
