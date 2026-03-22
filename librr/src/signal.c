#define _GNU_SOURCE

#include "rr_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct {
    void *start;
    size_t size;
    uint64_t utid;
} guard_page_t;

static stack_t g_altstack;
static int g_signalfd = -1;
static guard_page_t *g_guard_pages = NULL;
static size_t g_guard_len = 0;
static size_t g_guard_cap = 0;

static void segv_handler(int signo, siginfo_t *info, void *ucontext) {
    (void)signo;
    (void)ucontext;

    uint64_t utid = 0;
    if (rr_is_guard_page_fault(info->si_addr, &utid)) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg), "[rr] stack overflow detected in uthread %lu\n", (unsigned long)utid);
        if (n > 0) {
            syscall(SYS_write, STDERR_FILENO, msg, (size_t)n);
        }
        abort();
    }

    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

int rr_register_guard_page(void *guard_start, size_t guard_size, uint64_t utid) {
    if (g_guard_len == g_guard_cap) {
        size_t new_cap = g_guard_cap == 0 ? 16U : g_guard_cap * 2U;
        guard_page_t *new_pages = realloc(g_guard_pages, new_cap * sizeof(*new_pages));
        if (new_pages == NULL) {
            return -1;
        }
        g_guard_pages = new_pages;
        g_guard_cap = new_cap;
    }

    g_guard_pages[g_guard_len].start = guard_start;
    g_guard_pages[g_guard_len].size = guard_size;
    g_guard_pages[g_guard_len].utid = utid;
    g_guard_len += 1;
    return 0;
}

bool rr_is_guard_page_fault(void *addr, uint64_t *uthread_id_out) {
    uintptr_t fault = (uintptr_t)addr;
    for (size_t i = 0; i < g_guard_len; ++i) {
        uintptr_t start = (uintptr_t)g_guard_pages[i].start;
        uintptr_t end = start + g_guard_pages[i].size;
        if (fault >= start && fault < end) {
            if (uthread_id_out) {
                *uthread_id_out = g_guard_pages[i].utid;
            }
            return true;
        }
    }
    return false;
}

int rr_signal_init(void) {
    memset(&g_altstack, 0, sizeof(g_altstack));
    g_altstack.ss_size = 2U * SIGSTKSZ;
    g_altstack.ss_sp = mmap(NULL, g_altstack.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_altstack.ss_sp == MAP_FAILED) {
        g_altstack.ss_sp = NULL;
        return -1;
    }

    if (sigaltstack(&g_altstack, NULL) != 0) {
        munmap(g_altstack.ss_sp, g_altstack.ss_size);
        g_altstack.ss_sp = NULL;
        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        return -1;
    }

    sigset_t all;
    sigfillset(&all);
    if (sigprocmask(SIG_BLOCK, &all, NULL) != 0) {
        return -1;
    }

    g_signalfd = signalfd(-1, &all, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_signalfd < 0) {
        return -1;
    }

    return 0;
}

int rr_signal_fd(void) {
    return g_signalfd;
}

void rr_signal_process(struct signalfd_siginfo *info) {
    if (info == NULL) {
        return;
    }

    uint32_t active = rr_scheduler_current() ? (uint32_t)rr_scheduler_current()->id : 0;
    rr_log_signal(info->ssi_signo, active);

    if (info->ssi_signo == SIGINT || info->ssi_signo == SIGTERM) {
        rr_record_shutdown();
        _exit(0);
    }
}

void rr_signal_shutdown(void) {
    if (g_signalfd >= 0) {
        close(g_signalfd);
        g_signalfd = -1;
    }

    if (g_altstack.ss_sp) {
        munmap(g_altstack.ss_sp, g_altstack.ss_size);
        g_altstack.ss_sp = NULL;
    }

    free(g_guard_pages);
    g_guard_pages = NULL;
    g_guard_len = 0;
    g_guard_cap = 0;
}
