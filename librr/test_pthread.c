/*
 * test_pthread.c
 *
 * This is the baseline / comparison version using real pthreads.
 * It performs **exactly** the same amount of work as your uthread + RR test.
 *
 * Compile:
 *     gcc -Wall -O2 -o test_pthread test_pthread.c -pthread
 *
 * Run examples:
 *     ./test_pthread
 *     RR_TEST_LOOPS=50000 ./test_pthread
 *
 * Compare the "Elapsed time" with your RR version.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

void *worker(void *arg) {
    (void)arg;

    const char *loops_env = getenv("RR_TEST_LOOPS");
    int loops = 25000;
    if (loops_env && loops_env[0] != '\0') {
        int parsed = atoi(loops_env);
        if (parsed > 0) {
            loops = parsed;
        }
    }

    for (int i = 0; i < loops; i++) {
        /* Same fake CPU work */
        volatile long sum = 0;
        for (int j = 0; j < 1000; j++) {
            sum += j;
        }

        /* Same syscalls as in your RR test (but real ones, no interception) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        struct timeval tv;
        gettimeofday(&tv, NULL);

        getpid();
        getuid();

        char dummy[8] = {0};
        ssize_t r0 = read(STDIN_FILENO, dummy, 0);
        ssize_t w0 = write(STDOUT_FILENO, dummy, 0);
        (void)r0;
        (void)w0;
    }

    return NULL;
}

int main(void) {
    printf("=== Pthread Baseline Test ===\n");
    printf("(no uthreads, no io_uring, no syscall interception/logging)\n\n");

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    const int N_THREADS = 8;
    pthread_t threads[N_THREADS];

    /* Create threads */
    for (int i = 0; i < N_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }

    /* Join all */
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Elapsed time: %.4f seconds\n", elapsed);
    printf("=== Test finished ===\n\n");

    return 0;
}