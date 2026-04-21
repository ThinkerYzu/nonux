# Host-side test build. Uses the system cc (not the cross-compiler) so tests
# run on the dev machine with libc and a debugger attached.

CC       ?= cc
PYTHON   ?= python3
CFLAGS   := -Wall -Wextra -Werror -O0 -g -I../.. -I.
LDFLAGS  :=
GENCONF  := ../../tools/gen-config.py

# Tests + infrastructure:
SRCS := main.c \
        test_runner.c \
        mem_track.c \
        pmm_test.c \
        registry_test.c \
        component_test.c \
        component_deps_test.c \
        component_ipc_test.c \
        component_hook_test.c \
        component_pause_test.c \
        component_destroy_test.c \
        task_test.c

# Code-under-test pulled from the kernel tree:
SRCS += ../../core/pmm/pmm.c \
        ../../core/lib/string.c \
        ../../framework/registry.c \
        ../../framework/component.c \
        ../../framework/ipc.c \
        ../../framework/hook.c \
        ../../core/sched/task.c \
        ../../core/sched/sched.c

# Look up .c sources only (not .o) in the kernel source tree, so stale
# cross-compiled object files from `make` at the top don't get pulled in.
vpath %.c ../../core/pmm ../../core/lib ../../framework ../../core/sched

OBJS := $(notdir $(SRCS:.c=.o))

# Generated headers for slice-3.4 dep-injection tests.  `component_deps_test.c`
# consumes these via `#include "gen/<name>_deps.h"` — GCC's quote-form search
# finds the header relative to the including file's directory (./gen/...).
GEN_HEADERS := gen/sample_deps.h gen/trivial_deps.h

all: run

test_host: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

component_deps_test.o: $(GEN_HEADERS)

gen/%_deps.h: fixtures/%_deps.json $(GENCONF)
	@mkdir -p gen
	$(PYTHON) $(GENCONF) manifest $< gen/

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: test_host
	./test_host

clean:
	rm -rf test_host *.o gen/

.PHONY: all run clean
