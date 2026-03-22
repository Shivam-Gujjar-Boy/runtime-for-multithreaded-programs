#define _GNU_SOURCE

#include "rr_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RR_SCHED_STACK_SIZE (1U * 1024U * 1024U)

static uthread_t *g_runq_head = NULL;
static uthread_t *g_runq_tail = NULL;
static uthread_t *g_current = NULL;

static ucontext_t g_sched_ctx;
static void *g_sched_stack = NULL;
static bool g_sched_running = false;

static uthread_t **g_threads = NULL;
static size_t g_threads_len = 0;
static size_t g_threads_cap = 0;

static uthread_t *runq_pop_front(void) {
    uthread_t *ut = g_runq_head;
    if (ut == NULL) {
        return NULL;
    }

    g_runq_head = ut->next;
    if (g_runq_head == NULL) {
        g_runq_tail = NULL;
    }
    ut->next = NULL;
    return ut;
}

void rr_scheduler_enqueue(uthread_t *ut) {
    if (ut == NULL) {
        return;
    }

    ut->next = NULL;
    if (g_runq_tail == NULL) {
        g_runq_head = ut;
        g_runq_tail = ut;
        return;
    }

    g_runq_tail->next = ut;
    g_runq_tail = ut;
}

uthread_t *rr_scheduler_current(void) {
    return g_current;
}

void rr_scheduler_set_current(uthread_t *ut) {
    g_current = ut;
}

uthread_t *rr_scheduler_find(uint64_t id) {
    for (size_t i = 0; i < g_threads_len; ++i) {
        if (g_threads[i] && g_threads[i]->id == id) {
            return g_threads[i];
        }
    }
    return NULL;
}

int rr_scheduler_register_thread(uthread_t *ut) {
    if (ut == NULL) {
        return -1;
    }

    if (g_threads_len == g_threads_cap) {
        size_t new_cap = g_threads_cap == 0 ? 16U : g_threads_cap * 2U;
        uthread_t **new_list = realloc(g_threads, new_cap * sizeof(*new_list));
        if (new_list == NULL) {
            return -1;
        }
        g_threads = new_list;
        g_threads_cap = new_cap;
    }

    g_threads[g_threads_len++] = ut;
    return 0;
}

void rr_scheduler_unregister_all(void) {
    free(g_threads);
    g_threads = NULL;
    g_threads_len = 0;
    g_threads_cap = 0;
}

void rr_scheduler_wake_by_id(uint64_t id, int32_t io_result, const void *data, uint32_t data_len) {
    uthread_t *ut = rr_scheduler_find(id);
    if (ut == NULL || ut->state != UT_BLOCKED) {
        return;
    }

    ut->io_result = io_result;
    ut->io_data_len = 0;
    if (data && data_len > 0) {
        uint32_t copy_len = data_len > sizeof(ut->io_data) ? (uint32_t)sizeof(ut->io_data) : data_len;
        memcpy(ut->io_data, data, copy_len);
        ut->io_data_len = copy_len;
    }

    ut->state = UT_RUNNABLE;
    rr_scheduler_enqueue(ut);
}

static void scheduler_loop(void) {
    rr_set_fs_base(rr_scheduler_tls_base());

    while (g_sched_running) {
        uthread_t *next = runq_pop_front();
        if (next == NULL) {
            if (rr_io_wait() != 0 && errno == EINTR) {
                continue;
            }
            continue;
        }

        if (next->state != UT_RUNNABLE) {
            continue;
        }

        uint64_t from_id = g_current ? g_current->id : 0;
        uint64_t to_id = next->id;
        if (from_id != to_id) {
            rr_log_sched(from_id, to_id);
        }

        g_current = next;
        g_current->state = UT_RUNNING;
        rr_set_fs_base(g_current->tls_block);

        swapcontext(&g_sched_ctx, &g_current->ctx);

        rr_set_fs_base(rr_scheduler_tls_base());
    }
}

int rr_scheduler_init(void) {
    g_sched_stack = mmap(NULL, RR_SCHED_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_sched_stack == MAP_FAILED) {
        g_sched_stack = NULL;
        return -1;
    }

    if (getcontext(&g_sched_ctx) != 0) {
        munmap(g_sched_stack, RR_SCHED_STACK_SIZE);
        g_sched_stack = NULL;
        return -1;
    }

    g_sched_ctx.uc_stack.ss_sp = g_sched_stack;
    g_sched_ctx.uc_stack.ss_size = RR_SCHED_STACK_SIZE;
    g_sched_ctx.uc_link = NULL;
    makecontext(&g_sched_ctx, scheduler_loop, 0);

    g_sched_running = true;
    return 0;
}

void rr_scheduler_shutdown(void) {
    g_sched_running = false;
    g_runq_head = NULL;
    g_runq_tail = NULL;

    if (g_sched_stack) {
        munmap(g_sched_stack, RR_SCHED_STACK_SIZE);
        g_sched_stack = NULL;
    }

    rr_scheduler_unregister_all();
}

void rr_scheduler_yield_current(void) {
    uthread_t *cur = g_current;
    if (cur == NULL || !g_sched_running) {
        return;
    }

    cur->state = UT_RUNNABLE;
    rr_scheduler_enqueue(cur);
    swapcontext(&cur->ctx, &g_sched_ctx);
}

void rr_scheduler_block_current(void) {
    uthread_t *cur = g_current;
    if (cur == NULL || !g_sched_running) {
        return;
    }

    cur->state = UT_BLOCKED;
    swapcontext(&cur->ctx, &g_sched_ctx);
}

void rr_scheduler_exit_current(void *retval) {
    uthread_t *cur = g_current;
    if (cur == NULL || !g_sched_running) {
        return;
    }

    cur->retval = retval;
    cur->state = UT_DONE;

    if (cur->join_waiter && cur->join_waiter->state == UT_BLOCKED) {
        cur->join_waiter->state = UT_RUNNABLE;
        rr_scheduler_enqueue(cur->join_waiter);
    }

    swapcontext(&cur->ctx, &g_sched_ctx);
}
