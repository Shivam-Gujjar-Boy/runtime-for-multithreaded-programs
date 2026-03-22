#ifndef RR_H
#define RR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t uthread_create(void (*fn)(void *), void *arg);
void uthread_yield(void);
void uthread_exit(void *retval);
int uthread_join(uint64_t id, void **out);

#ifdef __cplusplus
}
#endif

#endif
