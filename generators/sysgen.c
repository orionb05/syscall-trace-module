#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#ifdef GEN_FUTEX
static int fut = 0;
static uint64_t end;

static void* futex_waiter(void* arg) {
    while (now_ns() < end) {
        syscall(SYS_futex, &fut, FUTEX_WAIT, 0, NULL, NULL, 0);
    }
    return NULL;
}
#endif

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <run-seconds>\n", argv[0]);
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds <= 0) {
        fprintf(stderr, "Invalid duration\n");
        return 1;
    }

    uint64_t end = now_ns() + (uint64_t)seconds * 1000000000ULL;

#ifdef GEN_READ
    int fd = open("/dev/zero", O_RDONLY);
    char buf[4096];
    while (now_ns() < end)
        read(fd, buf, sizeof(buf));
#endif

#ifdef GEN_MMAP
    while (now_ns() < end) {
        void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        munmap(p, 4096);
    }
#endif

#ifdef GEN_FUTEX
    end = now_ns() + (uint64_t)seconds * 1000000000ULL;

    pthread_t t;
    pthread_create(&t, NULL, futex_waiter, NULL);

    while (now_ns() < end) {
        syscall(SYS_futex, &fut, FUTEX_WAKE, 1, NULL, NULL, 0);
    }

    pthread_cancel(t);
    pthread_join(t, NULL);
#endif

#ifdef GEN_OPENAT
    while (now_ns() < end) {
        int fd = openat(AT_FDCWD, "/tmp/sysgen_tmp", O_CREAT|O_RDWR, 0600);
        close(fd);
        unlink("/tmp/sysgen_tmp");
    }
#endif

    return 0;
}