#define _GNU_SOURCE

#include "rr_internal.h"

#include <asm/prctl.h>
#include <errno.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint64_t g_next_id = 1;
static void *g_scheduler_tls = NULL;

static uthread_t **g_allocated = NULL;
static size_t g_allocated_len = 0;
static size_t g_allocated_cap = 0;

static int remember_thread(uthread_t *ut) {
    if (g_allocated_len == g_allocated_cap) {
        size_t new_cap = g_allocated_cap == 0 ? 16U : g_allocated_cap * 2U;
        uthread_t **new_items = realloc(g_allocated, new_cap * sizeof(*new_items));
        if (new_items == NULL) {
            return -1;
        }
        g_allocated = new_items;
        g_allocated_cap = new_cap;
    }

    g_allocated[g_allocated_len++] = ut;
    return 0;
}

int rr_set_fs_base(void *base) {
    return (int)syscall(SYS_arch_prctl, ARCH_SET_FS, base);
}

int rr_get_fs_base(void **base_out) {
    if (base_out == NULL) {
        return -1;
    }
    return (int)syscall(SYS_arch_prctl, ARCH_GET_FS, base_out);
}

void rr_set_scheduler_tls_base(void *base) {
    g_scheduler_tls = base;
}

void *rr_scheduler_tls_base(void) {
    return g_scheduler_tls;
}

static void uthread_entry(uintptr_t ut_ptr) {
    uthread_t *ut = (uthread_t *)ut_ptr;
    if (ut && ut->start_fn) {
        ut->start_fn(ut->start_arg);
    }
    uthread_exit(NULL);
}

int rr_uthread_init_main(void) {
    uthread_t *main_ut = calloc(1, sizeof(*main_ut));
    if (main_ut == NULL) {
        return -1;
    }

    main_ut->id = 1;
    main_ut->state = UT_RUNNING;

    if (getcontext(&main_ut->ctx) != 0) {
        free(main_ut);
        return -1;
    }

    void *fs_base = NULL;
    if (rr_get_fs_base(&fs_base) != 0) {
        free(main_ut);
        return -1;
    }

    main_ut->tls_block = fs_base;

    rr_scheduler_set_current(main_ut);
    rr_set_scheduler_tls_base(fs_base);

    if (rr_scheduler_register_thread(main_ut) != 0 || remember_thread(main_ut) != 0) {
        free(main_ut);
        return -1;
    }

    g_next_id = 2;
    return 0;
}

static int init_tls_copy(uthread_t *ut) {
    void *fs_base = NULL;
    if (rr_get_fs_base(&fs_base) != 0) {
        return -1;
    }

    ut->tls_block = fs_base;
    return 0;
}

int rr_uthread_create(void (*fn)(void *), void *arg, uint64_t *id_out) {
    if (fn == NULL || id_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    uthread_t *ut = calloc(1, sizeof(*ut));
    if (ut == NULL) {
        return -1;
    }

    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_size = pagesz + g_rr_config.stack_size;

    void *mem = mmap(NULL, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        free(ut);
        return -1;
    }

    if (mprotect((char *)mem + pagesz, g_rr_config.stack_size, PROT_READ | PROT_WRITE) != 0) {
        munmap(mem, map_size);
        free(ut);
        return -1;
    }

    ut->id = g_next_id++;
    ut->state = UT_RUNNABLE;
    ut->stack = (char *)mem + pagesz;
    ut->stack_size = g_rr_config.stack_size;
    ut->start_fn = fn;
    ut->start_arg = arg;
    ut->stack_map = mem;
    ut->stack_map_size = map_size;

    if (init_tls_copy(ut) != 0) {
        munmap(mem, map_size);
        free(ut);
        return -1;
    }

    if (getcontext(&ut->ctx) != 0) {
        munmap(mem, map_size);
        free(ut);
        return -1;
    }

    ut->ctx.uc_stack.ss_sp = ut->stack;
    ut->ctx.uc_stack.ss_size = ut->stack_size;
    ut->ctx.uc_link = NULL;
    makecontext(&ut->ctx, (void (*)(void))uthread_entry, 1, (uintptr_t)ut);

    if (rr_register_guard_page(mem, pagesz, ut->id) != 0 || rr_scheduler_register_thread(ut) != 0 || remember_thread(ut) != 0) {
        munmap(mem, map_size);
        free(ut);
        return -1;
    }

    rr_scheduler_enqueue(ut);
    *id_out = ut->id;
    return 0;
}

int rr_uthread_join(uint64_t id, void **out) {
    uthread_t *target = rr_scheduler_find(id);
    uthread_t *self = rr_scheduler_current();

    if (target == NULL) {
        errno = ESRCH;
        return -1;
    }
    if (self != NULL && target->id == self->id) {
        errno = EDEADLK;
        return -1;
    }

    if (target->state != UT_DONE) {
        if (self == NULL) {
            errno = EINVAL;
            return -1;
        }
        if (target->join_waiter != NULL && target->join_waiter != self) {
            errno = EBUSY;
            return -1;
        }

        target->join_waiter = self;
        rr_scheduler_block_current();
    }

    if (out) {
        *out = target->retval;
    }
    return 0;
}

void rr_uthread_shutdown(void) {
    for (size_t i = 0; i < g_allocated_len; ++i) {
        uthread_t *ut = g_allocated[i];
        if (ut == NULL) {
            continue;
        }

        if (ut->stack_map && ut->stack_map_size > 0) {
            munmap(ut->stack_map, ut->stack_map_size);
        }
        free(ut);
    }

    free(g_allocated);
    g_allocated = NULL;
    g_allocated_len = 0;
    g_allocated_cap = 0;
}

uint64_t uthread_create(void (*fn)(void *), void *arg) {
    if (!rr_active()) {
        errno = ENOSYS;
        return 0;
    }

    uint64_t id = 0;
    if (rr_uthread_create(fn, arg, &id) != 0) {
        return 0;
    }
    return id;
}

void uthread_yield(void) {
    if (!rr_active()) {
        return;
    }
    rr_scheduler_yield_current();
}

void uthread_exit(void *retval) {
    if (!rr_active()) {
        _exit(0);
    }

    rr_scheduler_exit_current(retval);
    __builtin_unreachable();
}

int uthread_join(uint64_t id, void **out) {
    if (!rr_active()) {
        errno = ENOSYS;
        return -1;
    }
    return rr_uthread_join(id, out);
}
