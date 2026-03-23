#define _GNU_SOURCE

#include "rr_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int g_replay_fd = -1;
static uint64_t g_expected_seq = 1;
static bool g_replay_header_ok = false;

static ssize_t (*g_real_read)(int, void *, size_t) = NULL;
static int (*g_real_open)(const char *, int, ...) = NULL;
static int (*g_real_close)(int) = NULL;

static int rr_replay_resolve_real_io(void) {
    if (g_real_read == NULL) {
        g_real_read = (ssize_t(*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    }
    if (g_real_open == NULL) {
        g_real_open = (int(*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    }
    if (g_real_close == NULL) {
        g_real_close = (int(*)(int))dlsym(RTLD_NEXT, "close");
    }

    return (g_real_read && g_real_open && g_real_close) ? 0 : -1;
}

static int rr_read_full(int fd, void *buf, size_t len) {
    uint8_t *ptr = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t rc = g_real_read(fd, ptr + done, len - done);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

int rr_replay_init(const char *log_path) {
    if (rr_replay_resolve_real_io() != 0) {
        return -1;
    }

    g_replay_fd = g_real_open(log_path, O_RDONLY);
    if (g_replay_fd < 0) {
        return -1;
    }

    rr_log_header_t header;
    if (rr_read_full(g_replay_fd, &header, sizeof(header)) != 0) {
        g_real_close(g_replay_fd);
        g_replay_fd = -1;
        return -1;
    }

    if (header.magic != RR_LOG_MAGIC || header.version != RR_LOG_VERSION) {
        g_real_close(g_replay_fd);
        g_replay_fd = -1;
        errno = EINVAL;
        return -1;
    }

    g_expected_seq = 1;
    g_replay_header_ok = true;
    return 0;
}

void rr_replay_shutdown(void) {
    if (g_replay_fd >= 0 && g_real_close) {
        g_real_close(g_replay_fd);
    }
    g_replay_fd = -1;
    g_replay_header_ok = false;
    g_expected_seq = 1;
}

static int rr_replay_next_event(rr_event_t *out_ev) {
    if (!g_replay_header_ok || g_replay_fd < 0 || out_ev == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rr_read_full(g_replay_fd, out_ev, sizeof(*out_ev)) != 0) {
        errno = ENODATA;
        return -1;
    }

    if (out_ev->seq != g_expected_seq) {
        rr_debugf("[rr-replay] sequence mismatch expected=%lu got=%lu\n",
                  (unsigned long)g_expected_seq,
                  (unsigned long)out_ev->seq);
        errno = EPROTO;
        return -1;
    }
    g_expected_seq += 1U;
    return 0;
}

int rr_replay_expect_sched(uint64_t expected_from_id, uint64_t expected_to_id) {
    rr_event_t ev;
    if (rr_replay_next_event(&ev) != 0) {
        return -1;
    }

    if (ev.type != EV_SCHED) {
        rr_debugf("[rr-replay] expected EV_SCHED at seq=%lu got type=%u\n",
                  (unsigned long)ev.seq,
                  (unsigned)ev.type);
        errno = EPROTO;
        return -1;
    }

    if (ev.payload.sched.from_id != expected_from_id || ev.payload.sched.to_id != expected_to_id) {
        rr_debugf("[rr-replay] sched mismatch seq=%lu expected=(%lu->%lu) got=(%lu->%lu)\n",
                  (unsigned long)ev.seq,
                  (unsigned long)expected_from_id,
                  (unsigned long)expected_to_id,
                  (unsigned long)ev.payload.sched.from_id,
                  (unsigned long)ev.payload.sched.to_id);
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int rr_replay_next_syscall(int32_t expected_nr, uint64_t expected_uthread_id, rr_replay_syscall_event_t *out_ev) {
    rr_event_t ev;
    if (!g_replay_header_ok || g_replay_fd < 0 || out_ev == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rr_replay_next_event(&ev) != 0) {
        return -1;
    }

    if (ev.type != EV_SYSCALL) {
        rr_debugf("[rr-replay] expected EV_SYSCALL at seq=%lu got type=%u\n",
                  (unsigned long)ev.seq,
                  (unsigned)ev.type);
        errno = EPROTO;
        return -1;
    }

    if (ev.payload.syscall.nr != expected_nr) {
        rr_debugf("[rr-replay] syscall mismatch expected=%d got=%d seq=%lu\n",
                  expected_nr,
                  ev.payload.syscall.nr,
                  (unsigned long)ev.seq);
        errno = EPROTO;
        return -1;
    }

    if (ev.uthread_id != expected_uthread_id) {
        rr_debugf("[rr-replay] uthread mismatch for syscall seq=%lu expected=%lu got=%lu\n",
                  (unsigned long)ev.seq,
                  (unsigned long)expected_uthread_id,
                  (unsigned long)ev.uthread_id);
        errno = EPROTO;
        return -1;
    }

    memset(out_ev, 0, sizeof(*out_ev));
    out_ev->seq = ev.seq;
    out_ev->nr = ev.payload.syscall.nr;
    out_ev->retval = ev.payload.syscall.retval;
    out_ev->arg0 = ev.payload.syscall.arg0;
    out_ev->data_len = ev.payload.syscall.data_len;
    if (out_ev->data_len > sizeof(out_ev->data)) {
        out_ev->data_len = sizeof(out_ev->data);
    }
    memcpy(out_ev->data, ev.payload.syscall.data, out_ev->data_len);
    return 0;
}
