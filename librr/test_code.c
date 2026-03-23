/*
 * test_rr.c
 *
 * Compile once:
 *     gcc -Wall -O2 -o test_rr test_rr.c -Ilibrr/include -Llibrr -lrr -ldl
 *
 * Then run TWICE (exactly the same binary):

 * 1. Recording ON  (this is what you want to measure):
 *    RR_MODE=record ./test_rr
 *
 * 2. Recording OFF (baseline, no overhead):
 *    ./test_rr
 *
 * The program will clearly print which mode it is running in.
 * It uses many intercepted syscalls (clock_gettime, gettimeofday,
 * getpid, getuid, read(0 bytes), write(0 bytes)) so the recording
 * overhead is clearly visible.
 */

#define _GNU_SOURCE
#include "rr.h"
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

void worker(void *arg) {
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
        /* Some CPU work */
        volatile long sum = 0;
        for (int j = 0; j < 1000; j++) {
            sum += j;
        }

        /* Intercepted syscalls - these are the source of overhead */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        struct timeval tv;
        gettimeofday(&tv, NULL);

        getpid();
        getuid();

        char dummy[8] = {0};
        ssize_t r0 = read(STDIN_FILENO, dummy, 0);     // fast, but logged + goes through io_uring path
        ssize_t w0 = write(STDOUT_FILENO, dummy, 0);   // fast, but logged
        (void)r0;
        (void)w0;
    }
}

int main(void) {
    const char *mode = getenv("RR_MODE");
    bool is_recording = (mode && strcmp(mode, "record") == 0);
    bool is_replay = (mode && strcmp(mode, "replay") == 0);

    if (is_recording) {
        printf("=== RR Test - Mode: RECORD (io_uring + logging active) ===\n");
    } else if (is_replay) {
        printf("=== RR Test - Mode: REPLAY (from logged events) ===\n");
    } else {
        printf("=== RR Test - Mode: OFF (direct libc pass-through) ===\n");
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    const int N_THREADS = 8;
    uint64_t tids[N_THREADS];

    /* Create threads using our uthread library */
    for (int i = 0; i < N_THREADS; i++) {
        tids[i] = uthread_create(worker, (void *)(long)i);
        if (tids[i] == 0) {
            fprintf(stderr, "uthread_create failed\n");
            return 1;
        }
    }

    /* Join all */
    for (int i = 0; i < N_THREADS; i++) {
        uthread_join(tids[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Elapsed time: %.4f seconds\n", elapsed);
    printf("=== Test finished ===\n\n");

    return 0;
}