#ifndef RR_INTERNAL_H
#define RR_INTERNAL_H

#define _GNU_SOURCE

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <ucontext.h>

#include <liburing.h>

#include "rr.h"

typedef enum {
    UT_RUNNABLE = 0,
    UT_RUNNING,
    UT_BLOCKED,
    UT_DONE
} ut_state_t;

typedef struct uthread {
    uint64_t id;
    ut_state_t state;
    ucontext_t ctx;
    void *stack;
    size_t stack_size;
    void *tls_block;
    struct uthread *join_waiter;
    void *retval;
    struct uthread *next;
    int32_t io_result;
    uint32_t io_data_len;
    uint8_t io_data[64];
    void (*start_fn)(void *);
    void *start_arg;
    void *stack_map;
    size_t stack_map_size;
} uthread_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t start_time;
    uint32_t pid;
    uint32_t reserved;
} rr_log_header_t;

typedef enum {
    EV_SYSCALL = 1,
    EV_SIGNAL = 2,
    EV_SCHED = 3,
    EV_RDTSC = 4
} rr_event_type_t;

typedef struct {
    uint64_t seq;
    uint64_t uthread_id;
    rr_event_type_t type;
    uint32_t reserved;
    union {
        struct {
            int32_t nr;
            int32_t retval;
            uint64_t arg0;
            uint8_t data[64];
            uint32_t data_len;
        } syscall;
        struct {
            uint32_t signo;
            uint32_t active_uthread;
        } signal;
        struct {
            uint64_t from_id;
            uint64_t to_id;
        } sched;
    } payload;
} rr_event_t;

typedef struct {
    bool mode_record;
    bool mode_replay;
    bool replay_time_virtual;
    bool runtime_ready;
    bool debug;
    size_t stack_size;
    const char *log_path;
} rr_config_t;

#define RR_DEFAULT_STACK_SIZE (2U * 1024U * 1024U)
#define RR_LOG_MAGIC 0x52524C47U
#define RR_LOG_VERSION 1U
#define RR_SIGNAL_SENTINEL UINT64_MAX
#define RR_LOG_RING_SIZE (4U * 1024U * 1024U)
#define RR_TLS_CLONE_SIZE (4096U)

extern rr_config_t g_rr_config;
extern uint64_t g_rr_seq;

bool rr_active(void);
bool rr_replay_active(void);
void rr_mark_runtime_ready(void);

void rr_debugf(const char *fmt, ...);

int rr_record_init(const char *log_path);
void rr_record_shutdown(void);
void rr_record_flush(void);
void rr_log_syscall(int32_t nr, int32_t retval, uint64_t arg0, const void *data, uint32_t data_len);
void rr_log_signal(uint32_t signo, uint32_t active_uthread);
void rr_log_sched(uint64_t from_id, uint64_t to_id);

typedef struct {
    uint64_t seq;
    int32_t nr;
    int32_t retval;
    uint64_t arg0;
    uint8_t data[64];
    uint32_t data_len;
} rr_replay_syscall_event_t;

int rr_replay_init(const char *log_path);
void rr_replay_shutdown(void);
int rr_replay_next_syscall(int32_t expected_nr, uint64_t expected_uthread_id, rr_replay_syscall_event_t *out_ev);
int rr_replay_expect_sched(uint64_t expected_from_id, uint64_t expected_to_id);

int rr_io_init(unsigned entries);
void rr_io_shutdown(void);
int rr_io_submit_signal_read(int signalfd_fd);
int rr_io_wait(void);
int32_t rr_io_submit_read_and_wait(int fd, void *buf, size_t count);
int32_t rr_io_submit_write_and_wait(int fd, const void *buf, size_t count);
int32_t rr_io_submit_recv_and_wait(int sockfd, void *buf, size_t len, int flags);
int32_t rr_io_submit_send_and_wait(int sockfd, const void *buf, size_t len, int flags);
int32_t rr_io_submit_accept_and_wait(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int32_t rr_io_submit_connect_and_wait(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int32_t rr_io_submit_openat_and_wait(int dirfd, const char *pathname, int flags, mode_t mode);
int32_t rr_io_submit_close_and_wait(int fd);
int32_t rr_io_submit_timerfd_read_and_wait(int fd, uint64_t *expirations);

int rr_signal_init(void);
void rr_signal_shutdown(void);
int rr_signal_fd(void);
void rr_signal_process(struct signalfd_siginfo *info);
bool rr_is_guard_page_fault(void *addr, uint64_t *uthread_id_out);
int rr_register_guard_page(void *guard_start, size_t guard_size, uint64_t utid);

int rr_scheduler_init(void);
void rr_scheduler_shutdown(void);
void rr_scheduler_block_current(void);
void rr_scheduler_yield_current(void);
void rr_scheduler_exit_current(void *retval);
void rr_scheduler_enqueue(uthread_t *ut);
uthread_t *rr_scheduler_current(void);
void rr_scheduler_set_current(uthread_t *ut);
void rr_scheduler_wake_by_id(uint64_t id, int32_t io_result, const void *data, uint32_t data_len);
uthread_t *rr_scheduler_find(uint64_t id);
int rr_scheduler_register_thread(uthread_t *ut);
void rr_scheduler_unregister_all(void);

int rr_uthread_init_main(void);
void rr_uthread_shutdown(void);
int rr_uthread_create(void (*fn)(void *), void *arg, uint64_t *id_out);
int rr_uthread_join(uint64_t id, void **out);

int rr_set_fs_base(void *base);
int rr_get_fs_base(void **base_out);
void rr_set_scheduler_tls_base(void *base);
void *rr_scheduler_tls_base(void);

#endif
