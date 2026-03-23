#define _GNU_SOURCE

#include "rr_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void rr_parse_env(void) {
    const char *mode = getenv("RR_MODE");
    const char *log_path = getenv("RR_LOG");
    const char *debug = getenv("RR_DEBUG");
    const char *replay_time = getenv("RR_REPLAY_TIME");
    const char *stack = getenv("RR_STACK_SIZE");

    g_rr_config.mode_record = (mode != NULL && strcmp(mode, "record") == 0);
    g_rr_config.mode_replay = (mode != NULL && strcmp(mode, "replay") == 0);
    g_rr_config.replay_time_virtual = !(replay_time != NULL && strcmp(replay_time, "real") == 0);
    g_rr_config.log_path = (log_path && log_path[0] != '\0') ? log_path : "rr.log";
    g_rr_config.debug = (debug != NULL && strcmp(debug, "1") == 0);

    g_rr_config.stack_size = RR_DEFAULT_STACK_SIZE;
    if (stack && stack[0] != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(stack, &end, 10);
        if (errno == 0 && end != stack && *end == '\0' && parsed > 0U) {
            g_rr_config.stack_size = (size_t)parsed;
        }
    }
}

__attribute__((constructor)) static void rr_init(void) {
    rr_parse_env();
    if (g_rr_config.mode_record) {
        if (rr_record_init(g_rr_config.log_path) != 0) {
            return;
        }

        if (rr_signal_init() != 0) {
            rr_record_shutdown();
            return;
        }

        if (rr_io_init(256U) != 0) {
            rr_signal_shutdown();
            rr_record_shutdown();
            return;
        }

        if (rr_io_submit_signal_read(rr_signal_fd()) != 0) {
            rr_io_shutdown();
            rr_signal_shutdown();
            rr_record_shutdown();
            return;
        }
    }

    if (g_rr_config.mode_replay) {
        if (rr_replay_init(g_rr_config.log_path) != 0) {
            return;
        }
    }

    if (rr_scheduler_init() != 0) {
        if (g_rr_config.mode_record) {
            rr_io_shutdown();
            rr_signal_shutdown();
            rr_record_shutdown();
        }
        if (g_rr_config.mode_replay) {
            rr_replay_shutdown();
        }
        return;
    }

    if (rr_uthread_init_main() != 0) {
        rr_scheduler_shutdown();
        if (g_rr_config.mode_record) {
            rr_io_shutdown();
            rr_signal_shutdown();
            rr_record_shutdown();
        }
        if (g_rr_config.mode_replay) {
            rr_replay_shutdown();
        }
        return;
    }

    g_rr_seq = 0;
    if (g_rr_config.mode_record) {
        rr_log_sched(0, 1);
    }
    if (g_rr_config.mode_replay) {
        if (rr_replay_expect_sched(0, 1) != 0) {
            rr_uthread_shutdown();
            rr_scheduler_shutdown();
            rr_replay_shutdown();
            return;
        }
    }
    rr_mark_runtime_ready();

    rr_debugf("[rr] initialized (record=%d replay=%d replay_time=%s)\n",
              g_rr_config.mode_record ? 1 : 0,
              g_rr_config.mode_replay ? 1 : 0,
              g_rr_config.replay_time_virtual ? "virtual" : "real");
}

__attribute__((destructor)) static void rr_shutdown(void) {
    if (!g_rr_config.runtime_ready) {
        return;
    }

    g_rr_config.runtime_ready = false;

    rr_uthread_shutdown();
    rr_scheduler_shutdown();
    if (g_rr_config.mode_record) {
        rr_io_shutdown();
        rr_signal_shutdown();
        rr_record_shutdown();
    }
    if (g_rr_config.mode_replay) {
        rr_replay_shutdown();
    }
}
