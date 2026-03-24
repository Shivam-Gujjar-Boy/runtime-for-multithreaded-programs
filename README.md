# librr: Deterministic Record and Replay Runtime for User-Level Threading

`librr` is a userland record-replay system for C programs that implements deterministic execution through cooperative user-level threading (M:1 threading model) and asynchronous I/O. It operates entirely in userspace without requiring kernel modifications or hardware virtualization.

## Table of Contents

1. [Overview](#overview)
2. [The Problem: Blocking System Calls in User Threads](#the-problem-blocking-system-calls-in-user-threads)
3. [The Solution: Asynchronous I/O with io_uring](#the-solution-asynchronous-io-with-iouring)
4. [Architecture](#architecture)
5. [Recording Mode](#recording-mode)
6. [Replay Mode](#replay-mode)
7. [Design Decisions](#design-decisions)
8. [Components](#components)
9. [Limitations](#limitations)
10. [Build and Usage](#build-and-usage)

---

## Overview

Traditional multithreaded applications use kernel threads (1:1 mapping), where the kernel schedules threads and handles blocking system calls. This library implements an M:1 threading model where multiple user threads (uthreads) run on a single kernel thread, with the runtime handling all scheduling decisions in userspace.

The key challenge with M:1 threading is that when a uthread makes a blocking system call, the kernel blocks the entire process (since it sees only one kernel thread), killing concurrency. This library solves this by converting blocking system calls into asynchronous io_uring operations, allowing the runtime to yield to other uthreads while I/O is pending.

### Key Features

- **Pure Userspace**: No kernel modifications, runs on stock Linux
- **Deterministic Replay**: Exact reproduction of execution including thread interleavings
- **Cooperative Scheduling**: FIFO scheduler with explicit yield points
- **Comprehensive Syscall Interception**: Uses LD_PRELOAD to intercept libc calls
- **Signal Handling**: Asynchronous signal delivery via signalfd
- **Stack Protection**: Guard pages for stack overflow detection

---

## The Problem: Blocking System Calls in User Threads

In a traditional green-thread (M:1) implementation:

1. Multiple user threads run cooperatively on one kernel thread
2. The runtime schedules threads via context switching
3. When a thread calls `read()` on a pipe or file:
   - The kernel blocks the entire process
   - All user threads stop, not just the calling thread
   - Concurrency is destroyed

In a native pthread application, when thread A blocks on I/O, the kernel schedules thread B. In M:1 threading without async I/O, blocking any thread blocks everything.

### The Interrupt Problem

Similarly, signal delivery from the kernel would interrupt the entire process. Traditional approaches require complex signal masking and re-delivery mechanisms.

---

## The Solution: Asynchronous I/O with io_uring

This library uses Linux's io_uring mechanism to convert blocking system calls into asynchronous operations:

### How It Works

1. **Submission**: When a uthread calls `read()`, instead of invoking the syscall directly, the runtime:
   - Creates an io_uring submission queue entry (SQE)
   - Tags the SQE with the uthread's ID
   - Submits the operation to the kernel
   - Marks the uthread as BLOCKED
   - Yields to the scheduler

2. **Concurrent Execution**: While I/O is pending:
   - The scheduler runs other RUNNABLE uthreads
   - Application work continues concurrently
   - The kernel processes the I/O in parallel

3. **Completion**: When I/O completes:
   - io_uring places a completion queue entry (CQE)
   - The runtime wakes the waiting uthread
   - Sets the uthread's state to RUNNABLE
   - Enqueues it for scheduling
   - Stores the result (bytes read, error code, etc.)

4. **Result Delivery**: When the uthread is scheduled again:
   - It resumes after the yield point
   - Returns the actual syscall result
   - The application code is unaware of the async nature

### Signal Handling

Signals are handled via `signalfd`:
1. All signals are blocked via `sigprocmask()`
2. A `signalfd` receives signal notifications
3. The signalfd is integrated into io_uring
4. Signals are logged and can be processed asynchronously
5. The SIGSEGV handler runs on an alternate stack for stack overflow detection

---

## Architecture

### Thread Model: M:1 (Many User Threads to One Kernel Thread)

```
Application View:
  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │ Thread A│  │ Thread B│  │ Thread C│  │ Thread D│
  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘
       └─────────────┴─────────────┴─────────────┘
                         │
              ┌──────────┴──────────┐
              │   User Scheduler    │
              │  (FIFO Run Queue)   │
              └──────────┬──────────┘
                         │
Kernel View:     ┌───────┴───────┐
                 │  One Kernel   │
                 │    Thread     │
                 └───────────────┘
```

### Uthread State Machine

```
                    ┌──────────┐
            ┌───────│  DONE    │
            │       └──────────┘
            │            ▲
            │            │ exit
            │       ┌──────────┐
         ┌──┴──┐    │ RUNNING  │
    ┌────┤READY│◄───┤          │
    │    └──┬──┘    └────┬─────┘
    │       ▲            │
    │       │ yield      │ block on I/O
    │    ┌──┴──┐         │
    └───►│RUNNABLE       │
         └─────┘         │
                        ▼
                   ┌──────────┐
                   │ BLOCKED  │
                   │(I/O wait)│
                   └────┬─────┘
                        │
                        │ I/O complete
                        ▼
                   ┌──────────┐
                   │ RUNNABLE │
                   └──────────┘
```

### Context Switching

The runtime uses `ucontext` for portable context switching:

1. **Scheduler Context**: The scheduler runs in its own context with a dedicated stack
2. **Thread Contexts**: Each uthread has its own context and stack
3. **TLS Management**: Thread-local storage uses x86-64 FS segment register, switched via `arch_prctl(ARCH_SET_FS)` on every context switch
4. **Switch Process**: `swapcontext()` saves current register state and restores target state

### Memory Layout per Uthread

```
┌─────────────────────────────────┐
│  Guard Page (PROT_NONE)         │ ◄── Catches stack overflow
├─────────────────────────────────┤
│                                 │
│  Stack (READ|WRITE)             │
│  ┌─────────────┐                │
│  │   ...       │                │
│  │  Local vars │                │
│  │  Saved regs │                │
│  └─────────────┘                │
│                                 │
├─────────────────────────────────┤
│                                 │
│  Uthread Control Block          │
│  (malloc'd separately)          │
│  - ID, state                    │
│  - ucontext                     │
│  - io_result                    │
│  - join_waiter ptr              │
│                                 │
└─────────────────────────────────┘
```

---

## Recording Mode

When `RR_MODE=record` is set, the runtime captures a deterministic trace of execution.

### What Gets Recorded

1. **Thread Switches (EV_SCHED)**: Every context switch is logged with source and destination thread IDs
2. **System Calls (EV_SYSCALL)**: For intercepted syscalls, the runtime logs:
   - Syscall number
   - Return value
   - First argument (for identification)
   - Up to 64 bytes of returned data (for read-like calls)
3. **Signals (EV_SIGNAL)**: Signal number and active thread when received

### Recording Workflow

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│ Application │───►│  Syscall    │───►│  io_uring   │
│   Code      │    │  Wrapper    │    │   Submit    │
└─────────────┘    └─────────────┘    └──────┬──────┘
       ▲                                     │
       │                                     │
       │    ┌─────────────┐    ┌────────────┴──────┐
       └───┐│   Return    │◄───│   Wait in CQ      │
           ││   Result    │    │   (async)         │
           │└─────────────┘    └────────────┬──────┘
       ┌───┘                               │
       │                              ┌────┴─────┐
       │                              │ Scheduler│
       │                              │ (yields  │
       │                              │  to other│
       │                              │  threads)│
       │                              └────┬─────┘
       │                                   │
       │                              ┌────┴─────┐
       └──────────────────────────────┤  CQE     │
                                      │ Complete │
                                      └────┬─────┘
                                           │
                                    ┌──────┴──────┐
                                    │ Log Event   │
                                    │ (EV_SYSCALL)│
                                    └──────┬──────┘
                                           │
                                    ┌──────┴──────┐
                                    │ Ring Buffer │
                                    │ (4MB)       │
                                    └─────────────┘
```

### Log Format

The log file consists of:
1. **Header**: Magic number, version, start time, PID
2. **Events**: Sequentially numbered events with type-specific payloads
3. **Buffering**: Events are batched in a userspace ring buffer and flushed to disk when full or on exit

The sequence number ensures event ordering and detects log corruption.

---

## Replay Mode

When `RR_MODE=replay` is set, the runtime reproduces the exact recorded execution.

### Replay Workflow

1. **Initialization**: Open the log file, validate header
2. **Scheduling Validation**: Every context switch is validated against the log
   - Expected: Thread 2 → Thread 3
   - Actual: Must match exactly or abort
3. **Syscall Reconstruction**: Instead of calling the kernel:
   - Read next event from log
   - Return recorded result value
   - Copy recorded data (for read-like calls)
   - Set errno from recorded error code

### Time Handling

The runtime supports two replay time modes:

- **Virtual Time (default)**: `clock_gettime()` and `gettimeofday()` return recorded values. This ensures deterministic timing and elapsed time calculations.
- **Real Time**: System calls return actual wall-clock time while still consuming log events. Useful for benchmarking replay overhead.

### Determinism Guarantees

Replay ensures:
- Same thread interleaving order
- Same syscall return values
- Same data returned by I/O operations
- Same timing (in virtual time mode)

Any deviation (wrong thread scheduled, wrong syscall, sequence mismatch) causes immediate abort.

---

## Design Decisions

### Why M:1 Threading?

**Advantages**:
- Complete control over scheduling decisions
- No kernel involvement in context switches (faster)
- Deterministic scheduling is possible
- Lower memory overhead per thread

**Trade-offs**:
- Cannot utilize multiple CPU cores (single kernel thread)
- Blocking syscalls must be async
- No true parallelism

### Why io_uring over other async I/O?

- **Unified interface**: Handles file I/O, network I/O, and timers
- **Efficient**: Shared ring buffers between user/kernel, minimal syscalls
- **Composable**: Can wait on I/O and signals simultaneously
- **Standard**: Native Linux API, no external dependencies

### Why LD_PRELOAD?

- **Non-intrusive**: Works with existing binaries without recompilation
- **Complete coverage**: Intercepts all libc calls that use wrapped symbols
- **Portable**: Standard mechanism on Linux

### Why ucontext instead of custom assembly?

- **Portability**: Works across x86-64 variants without assembly code
- **Maintainability**: Standard POSIX API
- **Adequate performance**: Context switches are not the bottleneck (I/O is)

### Why a ring buffer for logging?

- **Performance**: Batched writes reduce syscall overhead
- **Simplicity**: Single-producer, sequential access pattern
- **Reliability**: Memory-mapped, no malloc during logging path

---

## Components

### Scheduler (`scheduler.c`)

- FIFO run queue (singly-linked list)
- Round-robin scheduling of RUNNABLE threads
- Context switch coordination
- Recording of scheduling decisions

### io_uring Integration (`io_uring.c`)

- Submission of async operations (read, write, accept, connect, etc.)
- Completion handling and thread wakeups
- Signal fd integration
- Timer-based sleep (nanosleep via timerfd)

### Syscall Wrappers (`syscall_wrappers.c`)

Intercepted system calls with async variants:
- **I/O**: read, write, openat, close
- **Network**: recv, send, accept, connect
- **Time**: nanosleep (via timerfd), clock_gettime, gettimeofday
- **Process**: getpid, getuid, uname
- **Filesystem**: stat, fstat, lstat, getcwd

Each wrapper handles three modes: record (async + log), replay (return logged), passthrough (direct).

### Signal Handling (`signal.c`)

- Signal fd setup for async delivery
- SIGSEGV handler for guard page stack overflow detection
- Signal logging

### Uthread Management (`uthread.c`)

- Thread creation with guard-page protected stacks
- TLS base management
- Join/wait coordination
- Cleanup on exit

### Recording System (`record.c`)

- Event formatting and serialization
- Sequence number generation
- Ring buffer management
- Log file I/O

### Replay System (`replay.c`)

- Log file parsing and validation
- Event retrieval by type
- Syscall result reconstruction
- Schedule validation

---

## Limitations

### Fundamental Limitations

1. **Single Core**: M:1 threading cannot utilize multiple CPUs. The runtime pins to a single kernel thread.

2. **Syscall Coverage**: Only intercepted libc calls are handled. Direct syscall instructions (inline assembly, raw syscalls) bypass the runtime.

3. **vDSO Calls**: Some libc functions use vDSO to avoid syscalls entirely (e.g., optimized `gettimeofday` on some systems). These return real values during replay unless the vDSO is disabled.

4. **Memory-Mapped I/O**: Writes to shared memory from external processes are not tracked. Only syscalls made by the process are recorded.

### Not Yet Implemented

1. **RDTSC/RDRAND**: Hardware timestamp and random instructions are not intercepted. Code using these will diverge during replay.

2. **Signal Delivery**: Signals are logged but not re-delivered to user signal handlers during replay.

3. **Fork/Exec**: Child processes are not tracked. Forking creates a new process not under the runtime's control.

4. **Pthread Coexistence**: Mixing uthreads with real pthreads is unsupported and will cause crashes.

### Record/Replay Fidelity

1. **Data Truncation**: Read-like syscalls store only the first 64 bytes of returned data. Reads returning more than 64 bytes will have truncated data in the log.

2. **Pointer Arguments**: Syscalls that modify memory via pointers (e.g., `getsockopt`) may not have full data captured.

3. **External State**: Filesystem state, network state, and other external resources must be identical between record and replay.

4. **ASLR**: Position-independent executables may have different memory layouts; replay assumes deterministic memory addresses.

### Performance

1. **I/O Throughput**: io_uring adds some overhead compared to raw syscalls, though concurrency gains often outweigh this.

2. **Log Size**: High-frequency syscall applications generate large logs (4MB buffer helps, but flush frequency matters).

3. **Context Switch Cost**: User-level context switches are fast but not free; CPU-bound workloads may see overhead from cooperative scheduling.

---

## Build and Usage

### Prerequisites

- Linux kernel with io_uring support (5.1+)
- `liburing-dev` (Debian/Ubuntu) or `liburing-devel` (Fedora)
- Standard C compiler (gcc or clang)

### Build

```bash
cd librr
make              # Release build
make debug        # Debug build with AddressSanitizer
```

Outputs:
- `librr.so` - Release library
- `librr_debug.so` - Debug library

### Usage

#### Recording

```bash
LD_PRELOAD=/path/to/librr.so \
  RR_MODE=record \
  RR_LOG=/tmp/myapp.log \
  ./my_application
```

#### Replay

```bash
LD_PRELOAD=/path/to/librr.so \
  RR_MODE=replay \
  RR_LOG=/tmp/myapp.log \
  ./my_application
```

#### Passthrough (uthreads only, no recording)

```bash
LD_PRELOAD=/path/to/librr.so ./my_application
```

### Environment Variables

| Variable | Values | Description |
|----------|--------|-------------|
| `RR_MODE` | `record`, `replay` | Operation mode |
| `RR_LOG` | path | Log file path (default: `rr.log`) |
| `RR_REPLAY_TIME` | `virtual`, `real` | Time source in replay (default: `virtual`) |
| `RR_DEBUG` | `1`, `0` | Enable debug output (default: `0`) |
| `RR_STACK_SIZE` | bytes | Uthread stack size (default: 2MB) |

### Test

```bash
cd librr
make test
```

This runs a test harness that exercises thread creation, I/O operations, and join synchronization under record mode.

---

## Example: How Concurrency is Preserved

Consider a server handling two connections:

```c
void handler_a() {
    char buf[256];
    read(fd_a, buf, 256);  // Blocks for 100ms
    process(buf);
}

void handler_b() {
    char buf[256];
    read(fd_b, buf, 256);  // Blocks for 50ms
    process(buf);
}
```

**Traditional M:1 (without io_uring)**:
- Handler A calls read → kernel blocks process → handler B never runs
- Total time: 150ms (sequential)

**With librr (io_uring)**:
- Handler A submits read → yields → handler B runs
- Handler B submits read → yields → scheduler waits
- After 50ms, B completes → runs
- After 100ms, A completes → runs
- Total time: 100ms (concurrent)

The async approach provides true concurrency despite a single kernel thread.

---

## Summary

librr demonstrates that deterministic record-replay of multithreaded applications is achievable in userspace by:

1. Controlling all scheduling decisions through M:1 user threading
2. Converting blocking operations into asynchronous io_uring submissions
3. Interposing on libc calls to capture and reproduce behavior
4. Maintaining a sequential event log that validates replay correctness

The result is a system that can record complex multi-threaded interactions and replay them identically, useful for debugging, testing, and reproducible execution analysis.
