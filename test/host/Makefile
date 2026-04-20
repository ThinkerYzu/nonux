# Host-side test build. Uses the system cc (not the cross-compiler) so tests
# run on the dev machine with libc and a debugger attached.

CC      ?= cc
CFLAGS  := -Wall -Wextra -Werror -O0 -g -I../..
LDFLAGS :=

# Tests + infrastructure:
SRCS := main.c \
        test_runner.c \
        mem_track.c \
        pmm_test.c

# Code-under-test pulled from the kernel tree:
SRCS += ../../core/pmm/pmm.c \
        ../../core/lib/string.c

# Look up .c sources only (not .o) in the kernel source tree, so stale
# cross-compiled object files from `make` at the top don't get pulled in.
vpath %.c ../../core/pmm ../../core/lib

OBJS := $(notdir $(SRCS:.c=.o))

all: run

test_host: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: test_host
	./test_host

clean:
	rm -f test_host *.o

.PHONY: all run clean
