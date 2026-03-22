# RR Library (Record Phase)

This repository contains an RnD-quality deterministic record runtime (`librr.so`) for cooperative user-level threads (M:1), injected via `LD_PRELOAD`.

## Build

```bash
cd librr
make
```

Prerequisite: install liburing development headers (`liburing-dev` on Debian/Ubuntu).

Build outputs:
- `librr/librr.so` (release, `-O2 -g`)
- `librr/librr_debug.so` (`make debug`, with ASAN)

## Run (record mode)

```bash
LD_PRELOAD=/path/to/librr.so RR_MODE=record RR_LOG=/tmp/rr.log ./target_binary
```

Environment variables:
- `RR_MODE=record`: enables record mode. If unset or different, wrappers pass through.
- `RR_LOG=/path/to/file`: log path (default: `rr.log`)
- `RR_DEBUG=1`: emit debug traces to stderr
- `RR_STACK_SIZE=<bytes>`: uthread stack size (default: `2097152`)

## Source layout

```text
librr/
	include/
		rr.h
		rr_internal.h
	src/
		init.c
		uthread.c
		scheduler.c
		syscall_wrappers.c
		io_uring.c
		signal.c
		record.c
		log.c
	test/
		test_runtime.c
	Makefile
```

## Implemented behavior

- Cooperative M:1 runtime with FIFO run queue.
- `ucontext`-based context switches (`getcontext/makecontext/swapcontext`).
- Guard-page stack allocation for non-main uthreads via `mmap + mprotect`.
- Per-uthread TLS base switching with `arch_prctl(ARCH_SET_FS, ...)`.
- `io_uring` integration for blocking calls:
	- `read`, `write`, `recv`, `send`, `accept`, `connect`, `openat`, `close`, `nanosleep` (timerfd-based)
- Signal ingestion via `signalfd` (all blocked process signals), with persistent signalfd read SQE.
- `SIGSEGV` guard-page overflow detection via `sigaltstack` + `sigaction(SA_ONSTACK)`.
- Binary record log with:
	- header (`magic`, `version`, monotonic start ns, pid)
	- `EV_SYSCALL`, `EV_SIGNAL`, `EV_SCHED`
	- monotonic `rr_seq` starting at 1
- Log buffering with userspace ring buffer (4 MB), flush-on-full and flush-on-exit.

## Public API

Declared in `librr/include/rr.h`:
- `uint64_t uthread_create(void (*fn)(void *), void *arg);`
- `void uthread_yield(void);`
- `void uthread_exit(void *retval);`
- `int uthread_join(uint64_t id, void **out);`

## Test

```bash
cd librr
make test
```

This builds and runs a small harness that exercises create/yield/join and wrapped read/write paths under record mode.

## Explicitly not implemented yet (future replay phase)

- Replay-mode execution
- `rdtsc` / `rdrand` interception
- Delivery of logged signals to user handlers
- `fork` / `exec` support
- `pthread_create` coexistence

## Limitations

- Single-process only (`fork`, `exec`, `clone` unsupported).
- Libraries creating background pthreads are unsupported.
- Raw syscalls bypassing libc PLT are not intercepted.
- Direct vDSO calls that bypass intercepted libc symbols are not intercepted.
- External-process writes to mmap’d regions are not tracked.
- `rdtsc`/`rdrand`/`rdpmc` non-determinism is not handled in this phase.
- RT signals are logged but not re-delivered to target handlers.
- `setjmp`/`longjmp` across uthread boundaries is undefined.
- ASAN + `LD_PRELOAD` debug runs can be fragile.
