#include "rr.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_pipefd[2];

static void writer_thread(void *arg) {
    (void)arg;
    const char *msg = "rr-test-message";
    write(g_pipefd[1], msg, strlen(msg));
    uthread_exit((void *)1);
}

static void reader_thread(void *arg) {
    (void)arg;
    char buf[64] = {0};
    ssize_t n = read(g_pipefd[0], buf, sizeof(buf));
    if (n > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
        write(STDOUT_FILENO, "\n", 1);
    }
    uthread_exit((void *)2);
}

int main(void) {
    if (pipe(g_pipefd) != 0) {
        perror("pipe");
        return 1;
    }

    uint64_t writer = uthread_create(writer_thread, NULL);
    uint64_t reader = uthread_create(reader_thread, NULL);
    if (writer == 0 || reader == 0) {
        fprintf(stderr, "uthread_create failed\n");
        return 2;
    }

    void *ret1 = NULL;
    void *ret2 = NULL;
    if (uthread_join(writer, &ret1) != 0 || uthread_join(reader, &ret2) != 0) {
        fprintf(stderr, "uthread_join failed\n");
        return 3;
    }

    close(g_pipefd[0]);
    close(g_pipefd[1]);

    if (ret1 != (void *)1 || ret2 != (void *)2) {
        fprintf(stderr, "unexpected return values\n");
        return 4;
    }

    return 0;
}
