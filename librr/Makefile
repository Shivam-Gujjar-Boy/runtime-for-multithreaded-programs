CC ?= cc
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Werror -fPIC -Iinclude
LDFLAGS_SO := -shared -ldl -luring

SRC := \
	src/init.c \
	src/uthread.c \
	src/scheduler.c \
	src/syscall_wrappers.c \
	src/io_uring.c \
	src/signal.c \
	src/record.c \
	src/replay.c \
	src/log.c

OBJ_RELEASE := $(SRC:.c=.o)
OBJ_DEBUG := $(SRC:.c=.debug.o)

.PHONY: all debug clean test

all: librr.so

librr.so: $(OBJ_RELEASE)
	$(CC) -O2 -g -fPIC -shared -o $@ $(OBJ_RELEASE) $(LDFLAGS_SO)

%.o: %.c
	$(CC) $(CFLAGS_COMMON) -O2 -g -c $< -o $@

debug: CFLAGS_COMMON += -O0 -g3 -fsanitize=address

debug: LDFLAGS_SO += -fsanitize=address

debug: librr_debug.so

librr_debug.so: $(OBJ_DEBUG)
	$(CC) -O0 -g3 -fPIC -shared -o $@ $(OBJ_DEBUG) $(LDFLAGS_SO)

%.debug.o: %.c
	$(CC) $(CFLAGS_COMMON) -O0 -g3 -fsanitize=address -c $< -o $@

test: librr.so test/test_runtime
	LD_PRELOAD=$(PWD)/librr.so RR_MODE=record RR_LOG=$(PWD)/test/rr_test.log ./test/test_runtime

test/test_runtime: test/test_runtime.c librr.so
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude test/test_runtime.c -L. -lrr -Wl,-rpath,$(PWD) -o $@

clean:
	rm -f $(OBJ_RELEASE) $(OBJ_DEBUG) librr.so librr_debug.so test/test_runtime test/rr_test.log
