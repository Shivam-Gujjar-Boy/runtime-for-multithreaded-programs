#define _GNU_SOURCE

#include "rr_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

rr_config_t g_rr_config = {
    .mode_record = false,
    .mode_replay = false,
    .replay_time_virtual = true,
    .runtime_ready = false,
    .debug = false,
    .stack_size = RR_DEFAULT_STACK_SIZE,
    .log_path = "rr.log",
};

uint64_t g_rr_seq = 0;

static int g_log_fd = -1;
static uint8_t *g_ring = NULL;
static size_t g_ring_used = 0;

static ssize_t (*g_real_write)(int, const void *, size_t) = NULL;
static int (*g_real_open)(const char *, int, ...) = NULL;
static int (*g_real_close)(int) = NULL;

static uint64_t rr_now_mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static int rr_resolve_real_io(void) {
    if (g_real_write == NULL) {
        g_real_write = (ssize_t(*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    }
    if (g_real_open == NULL) {
        g_real_open = (int(*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    }
    if (g_real_close == NULL) {
        g_real_close = (int(*)(int))dlsym(RTLD_NEXT, "close");
    }
    return (g_real_write && g_real_open && g_real_close) ? 0 : -1;
}

bool rr_active(void) {
    return g_rr_config.mode_record && g_rr_config.runtime_ready;
}

bool rr_replay_active(void) {
    return g_rr_config.mode_replay && g_rr_config.runtime_ready;
}

void rr_mark_runtime_ready(void) {
    g_rr_config.runtime_ready = true;
}

static int rr_write_direct(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t rc = g_real_write(g_log_fd, p + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        written += (size_t)rc;
    }
    return 0;
}

void rr_record_flush(void) {
    if (g_log_fd < 0 || g_ring == NULL || g_ring_used == 0) {
        return;
    }

    if (__builtin_expect(rr_write_direct(g_ring, g_ring_used) != 0, 0)) {
        rr_debugf("[rr] failed flushing log buffer\n");
    }
    g_ring_used = 0;
}

static void rr_record_append(const void *entry, size_t len) {
    if (g_ring == NULL) {
        return;
    }

    if (len > RR_LOG_RING_SIZE) {
        rr_record_flush();
        rr_write_direct(entry, len);
        return;
    }

    if (g_ring_used + len > RR_LOG_RING_SIZE) {
        rr_record_flush();
    }

    memcpy(g_ring + g_ring_used, entry, len);
    g_ring_used += len;
}

int rr_record_init(const char *log_path) {
    if (rr_resolve_real_io() != 0) {
        return -1;
    }

    g_ring = mmap(NULL, RR_LOG_RING_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_ring == MAP_FAILED) {
        g_ring = NULL;
        return -1;
    }

    g_log_fd = g_real_open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_log_fd < 0) {
        munmap(g_ring, RR_LOG_RING_SIZE);
        g_ring = NULL;
        return -1;
    }

    rr_log_header_t header = {
        .magic = RR_LOG_MAGIC,
        .version = RR_LOG_VERSION,
        .start_time = rr_now_mono_ns(),
        .pid = (uint32_t)getpid(),
        .reserved = 0,
    };

    if (rr_write_direct(&header, sizeof(header)) != 0) {
        g_real_close(g_log_fd);
        g_log_fd = -1;
        munmap(g_ring, RR_LOG_RING_SIZE);
        g_ring = NULL;
        return -1;
    }

    return 0;
}

void rr_record_shutdown(void) {
    rr_record_flush();

    if (g_log_fd >= 0 && g_real_close) {
        g_real_close(g_log_fd);
        g_log_fd = -1;
    }
    if (g_ring) {
        munmap(g_ring, RR_LOG_RING_SIZE);
        g_ring = NULL;
    }
}

static uint64_t rr_next_seq(void) {
    g_rr_seq += 1U;
    return g_rr_seq;
}

void rr_log_syscall(int32_t nr, int32_t retval, uint64_t arg0, const void *data, uint32_t data_len) {
    if (!rr_active()) {
        return;
    }

    rr_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = rr_next_seq();
    ev.uthread_id = rr_scheduler_current() ? rr_scheduler_current()->id : 0;
    ev.type = EV_SYSCALL;
    ev.payload.syscall.nr = nr;
    ev.payload.syscall.retval = retval;
    ev.payload.syscall.arg0 = arg0;

    if (data != NULL && data_len > 0) {
        uint32_t capped = data_len > sizeof(ev.payload.syscall.data) ? (uint32_t)sizeof(ev.payload.syscall.data) : data_len;
        memcpy(ev.payload.syscall.data, data, capped);
        ev.payload.syscall.data_len = capped;
    }

    rr_record_append(&ev, sizeof(ev));
}

void rr_log_signal(uint32_t signo, uint32_t active_uthread) {
    if (!rr_active()) {
        return;
    }

    rr_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = rr_next_seq();
    ev.uthread_id = rr_scheduler_current() ? rr_scheduler_current()->id : 0;
    ev.type = EV_SIGNAL;
    ev.payload.signal.signo = signo;
    ev.payload.signal.active_uthread = active_uthread;

    rr_record_append(&ev, sizeof(ev));
}

void rr_log_sched(uint64_t from_id, uint64_t to_id) {
    if (!g_rr_config.mode_record) {
        return;
    }

    rr_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = rr_next_seq();
    ev.uthread_id = to_id;
    ev.type = EV_SCHED;
    ev.payload.sched.from_id = from_id;
    ev.payload.sched.to_id = to_id;

    rr_record_append(&ev, sizeof(ev));
}
