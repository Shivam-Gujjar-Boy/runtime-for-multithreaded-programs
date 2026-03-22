#define _GNU_SOURCE

#include "rr_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

void rr_debugf(const char *fmt, ...) {
    if (!g_rr_config.debug) {
        return;
    }

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }

    if ((size_t)n >= sizeof(buf)) {
        n = (int)(sizeof(buf) - 1U);
        buf[n] = '\0';
    }

    syscall(SYS_write, STDERR_FILENO, buf, (size_t)n);
}
