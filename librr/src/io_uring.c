#define _GNU_SOURCE

#include "rr_internal.h"

#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static struct io_uring g_ring;
static bool g_ring_ready = false;
static int g_signal_fd = -1;
static struct signalfd_siginfo g_signal_info;

static int submit_and_block(uthread_t *ut) {
    if (io_uring_submit(&g_ring) < 0) {
        return -1;
    }

    rr_scheduler_block_current();
    return ut->io_result;
}

int rr_io_init(unsigned entries) {
    memset(&g_ring, 0, sizeof(g_ring));
    if (io_uring_queue_init((unsigned)entries, &g_ring, 0) < 0) {
        return -1;
    }
    g_ring_ready = true;
    return 0;
}

void rr_io_shutdown(void) {
    if (g_ring_ready) {
        io_uring_queue_exit(&g_ring);
        g_ring_ready = false;
    }
}

int rr_io_submit_signal_read(int signalfd_fd) {
    if (!g_ring_ready) {
        errno = EINVAL;
        return -1;
    }

    g_signal_fd = signalfd_fd;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_read(sqe, signalfd_fd, &g_signal_info, sizeof(g_signal_info), 0);
    sqe->user_data = RR_SIGNAL_SENTINEL;

    return io_uring_submit(&g_ring) < 0 ? -1 : 0;
}

int rr_io_wait(void) {
    if (!g_ring_ready) {
        errno = EINVAL;
        return -1;
    }

    struct io_uring_cqe *cqe = NULL;
    int rc = io_uring_wait_cqe(&g_ring, &cqe);
    if (rc < 0 || cqe == NULL) {
        errno = -rc;
        return -1;
    }

    if (cqe->user_data == RR_SIGNAL_SENTINEL) {
        if (cqe->res >= (int)sizeof(struct signalfd_siginfo)) {
            rr_signal_process(&g_signal_info);
        }
        if (g_signal_fd >= 0) {
            rr_io_submit_signal_read(g_signal_fd);
        }
    } else {
        rr_scheduler_wake_by_id(cqe->user_data, cqe->res, NULL, 0);
    }

    io_uring_cqe_seen(&g_ring, cqe);
    return 0;
}

int32_t rr_io_submit_read_and_wait(int fd, void *buf, size_t count) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_read(sqe, fd, buf, count, 0);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_write_and_wait(int fd, const void *buf, size_t count) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_write(sqe, fd, buf, count, 0);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_recv_and_wait(int sockfd, void *buf, size_t len, int flags) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_recv(sqe, sockfd, buf, len, flags);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_send_and_wait(int sockfd, const void *buf, size_t len, int flags) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_send(sqe, sockfd, buf, len, flags);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_accept_and_wait(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_accept(sqe, sockfd, addr, addrlen, 0);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_connect_and_wait(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_connect(sqe, sockfd, addr, addrlen);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_openat_and_wait(int dirfd, const char *pathname, int flags, mode_t mode) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_openat(sqe, dirfd, pathname, flags, mode);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_close_and_wait(int fd) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_close(sqe, fd);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}

int32_t rr_io_submit_timerfd_read_and_wait(int fd, uint64_t *expirations) {
    uthread_t *ut = rr_scheduler_current();
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (ut == NULL || sqe == NULL) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_read(sqe, fd, expirations, sizeof(*expirations), 0);
    sqe->user_data = ut->id;

    return submit_and_block(ut);
}
