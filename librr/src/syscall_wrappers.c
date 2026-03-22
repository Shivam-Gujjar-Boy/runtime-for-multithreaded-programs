#define _GNU_SOURCE

#include "rr_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_stat
#define __NR_stat __NR_newfstatat
#endif

#ifndef __NR_lstat
#define __NR_lstat __NR_newfstatat
#endif

static int32_t to_log_int(int rc) {
    if (rc == -1) {
        return -errno;
    }
    return rc;
}

static ssize_t raw_to_ssize(int32_t raw) {
    if (raw < 0) {
        errno = -raw;
        return -1;
    }
    return (ssize_t)raw;
}

static int raw_to_int(int32_t raw) {
    if (raw < 0) {
        errno = -raw;
        return -1;
    }
    return (int)raw;
}

ssize_t read(int fd, void *buf, size_t count) {
    static ssize_t (*real_read)(int, void *, size_t) = NULL;
    if (!real_read) {
        real_read = (ssize_t(*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    }

    if (!rr_active()) {
        return real_read(fd, buf, count);
    }

    int32_t raw = rr_io_submit_read_and_wait(fd, buf, count);
    ssize_t ret = raw_to_ssize(raw);

    uint32_t len = raw > 0 ? (uint32_t)raw : 0;
    rr_log_syscall(__NR_read, raw, (uint64_t)fd, buf, len);
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    static ssize_t (*real_write)(int, const void *, size_t) = NULL;
    if (!real_write) {
        real_write = (ssize_t(*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    }

    if (!rr_active()) {
        return real_write(fd, buf, count);
    }

    int32_t raw = rr_io_submit_write_and_wait(fd, buf, count);
    ssize_t ret = raw_to_ssize(raw);
    rr_log_syscall(__NR_write, raw, (uint64_t)fd, NULL, 0);
    return ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    static ssize_t (*real_recv)(int, void *, size_t, int) = NULL;
    if (!real_recv) {
        real_recv = (ssize_t(*)(int, void *, size_t, int))dlsym(RTLD_NEXT, "recv");
    }

    if (!rr_active()) {
        return real_recv(sockfd, buf, len, flags);
    }

    int32_t raw = rr_io_submit_recv_and_wait(sockfd, buf, len, flags);
    ssize_t ret = raw_to_ssize(raw);

    uint32_t data_len = raw > 0 ? (uint32_t)raw : 0;
    rr_log_syscall(__NR_recvfrom, raw, (uint64_t)sockfd, buf, data_len);
    return ret;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
    if (!real_send) {
        real_send = (ssize_t(*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    }

    if (!rr_active()) {
        return real_send(sockfd, buf, len, flags);
    }

    int32_t raw = rr_io_submit_send_and_wait(sockfd, buf, len, flags);
    ssize_t ret = raw_to_ssize(raw);
    rr_log_syscall(__NR_sendto, raw, (uint64_t)sockfd, NULL, 0);
    return ret;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*real_accept)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!real_accept) {
        real_accept = (int(*)(int, struct sockaddr *, socklen_t *))dlsym(RTLD_NEXT, "accept");
    }

    if (!rr_active()) {
        return real_accept(sockfd, addr, addrlen);
    }

    int32_t raw = rr_io_submit_accept_and_wait(sockfd, addr, addrlen);
    int ret = raw_to_int(raw);
    rr_log_syscall(__NR_accept, raw, (uint64_t)sockfd, NULL, 0);
    return ret;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
    if (!real_connect) {
        real_connect = (int(*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    }

    if (!rr_active()) {
        return real_connect(sockfd, addr, addrlen);
    }

    int32_t raw = rr_io_submit_connect_and_wait(sockfd, addr, addrlen);
    int ret = raw_to_int(raw);
    rr_log_syscall(__NR_connect, raw, (uint64_t)sockfd, NULL, 0);
    return ret;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    static int (*real_openat)(int, const char *, int, ...) = NULL;
    if (!real_openat) {
        real_openat = (int(*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
    }

    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    if (!rr_active()) {
        if ((flags & O_CREAT) != 0) {
            return real_openat(dirfd, pathname, flags, mode);
        }
        return real_openat(dirfd, pathname, flags);
    }

    int32_t raw = rr_io_submit_openat_and_wait(dirfd, pathname, flags, mode);
    int ret = raw_to_int(raw);
    rr_log_syscall(__NR_openat, raw, (uint64_t)dirfd, NULL, 0);
    return ret;
}

int close(int fd) {
    static int (*real_close)(int) = NULL;
    if (!real_close) {
        real_close = (int(*)(int))dlsym(RTLD_NEXT, "close");
    }

    if (!rr_active()) {
        return real_close(fd);
    }

    int32_t raw = rr_io_submit_close_and_wait(fd);
    int ret = raw_to_int(raw);
    rr_log_syscall(__NR_close, raw, (uint64_t)fd, NULL, 0);
    return ret;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    static int (*real_nanosleep)(const struct timespec *, struct timespec *) = NULL;
    if (!real_nanosleep) {
        real_nanosleep = (int(*)(const struct timespec *, struct timespec *))dlsym(RTLD_NEXT, "nanosleep");
    }

    if (!rr_active()) {
        return real_nanosleep(req, rem);
    }

    int tfd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) {
        int32_t log_ret = -errno;
        rr_log_syscall(__NR_nanosleep, log_ret, 0, NULL, 0);
        return -1;
    }

    struct itimerspec spec;
    memset(&spec, 0, sizeof(spec));
    spec.it_value = *req;
    if (syscall(SYS_timerfd_settime, tfd, 0, &spec, NULL) < 0) {
        int err = errno;
        syscall(SYS_close, tfd);
        errno = err;
        rr_log_syscall(__NR_nanosleep, -err, 0, NULL, 0);
        return -1;
    }

    uint64_t expirations = 0;
    int32_t raw = rr_io_submit_timerfd_read_and_wait(tfd, &expirations);
    syscall(SYS_close, tfd);

    if (raw < 0) {
        errno = -raw;
        rr_log_syscall(__NR_nanosleep, raw, 0, NULL, 0);
        return -1;
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    rr_log_syscall(__NR_nanosleep, 0, 0, NULL, 0);
    return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    static int (*real_clock_gettime)(clockid_t, struct timespec *) = NULL;
    if (!real_clock_gettime) {
        real_clock_gettime = (int(*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    }

    int rc = real_clock_gettime(clock_id, tp);
    if (rr_active()) {
        rr_log_syscall(__NR_clock_gettime, to_log_int(rc), (uint64_t)clock_id, tp, rc == 0 ? (uint32_t)sizeof(*tp) : 0);
    }
    return rc;
}

int gettimeofday(struct timeval *tv, void *tz) {
    static int (*real_gettimeofday)(struct timeval *, void *) = NULL;
    if (!real_gettimeofday) {
        real_gettimeofday = (int(*)(struct timeval *, void *))dlsym(RTLD_NEXT, "gettimeofday");
    }

    int rc = real_gettimeofday(tv, tz);
    if (rr_active()) {
        rr_log_syscall(__NR_gettimeofday, to_log_int(rc), 0, tv, rc == 0 ? (uint32_t)sizeof(*tv) : 0);
    }
    return rc;
}

pid_t getpid(void) {
    static pid_t (*real_getpid)(void) = NULL;
    if (!real_getpid) {
        real_getpid = (pid_t(*)(void))dlsym(RTLD_NEXT, "getpid");
    }

    pid_t rc = real_getpid();
    if (rr_active()) {
        rr_log_syscall(__NR_getpid, (int32_t)rc, 0, NULL, 0);
    }
    return rc;
}

uid_t getuid(void) {
    static uid_t (*real_getuid)(void) = NULL;
    if (!real_getuid) {
        real_getuid = (uid_t(*)(void))dlsym(RTLD_NEXT, "getuid");
    }

    uid_t rc = real_getuid();
    if (rr_active()) {
        rr_log_syscall(__NR_getuid, (int32_t)rc, 0, NULL, 0);
    }
    return rc;
}

int uname(struct utsname *buf) {
    static int (*real_uname)(struct utsname *) = NULL;
    if (!real_uname) {
        real_uname = (int(*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
    }

    int rc = real_uname(buf);
    if (rr_active()) {
        rr_log_syscall(__NR_uname, to_log_int(rc), 0, buf, rc == 0 ? (uint32_t)sizeof(*buf) : 0);
    }
    return rc;
}

int stat(const char *path, struct stat *st) {
    static int (*real_stat)(const char *, struct stat *) = NULL;
    if (!real_stat) {
        real_stat = (int(*)(const char *, struct stat *))dlsym(RTLD_NEXT, "stat");
    }

    int rc = real_stat(path, st);
    if (rr_active()) {
        rr_log_syscall(__NR_stat, to_log_int(rc), (uint64_t)(uintptr_t)path, st, rc == 0 ? (uint32_t)sizeof(*st) : 0);
    }
    return rc;
}

int fstat(int fd, struct stat *st) {
    static int (*real_fstat)(int, struct stat *) = NULL;
    if (!real_fstat) {
        real_fstat = (int(*)(int, struct stat *))dlsym(RTLD_NEXT, "fstat");
    }

    int rc = real_fstat(fd, st);
    if (rr_active()) {
        rr_log_syscall(__NR_fstat, to_log_int(rc), (uint64_t)fd, st, rc == 0 ? (uint32_t)sizeof(*st) : 0);
    }
    return rc;
}

int lstat(const char *path, struct stat *st) {
    static int (*real_lstat)(const char *, struct stat *) = NULL;
    if (!real_lstat) {
        real_lstat = (int(*)(const char *, struct stat *))dlsym(RTLD_NEXT, "lstat");
    }

    int rc = real_lstat(path, st);
    if (rr_active()) {
        rr_log_syscall(__NR_lstat, to_log_int(rc), (uint64_t)(uintptr_t)path, st, rc == 0 ? (uint32_t)sizeof(*st) : 0);
    }
    return rc;
}

char *getcwd(char *buf, size_t size) {
    static char *(*real_getcwd)(char *, size_t) = NULL;
    if (!real_getcwd) {
        real_getcwd = (char *(*)(char *, size_t))dlsym(RTLD_NEXT, "getcwd");
    }

    char *rc = real_getcwd(buf, size);
    if (rr_active()) {
        int32_t retval = rc ? 0 : -errno;
        rr_log_syscall(__NR_getcwd, retval, size, rc, rc ? (uint32_t)strnlen(rc, size) : 0);
    }
    return rc;
}
