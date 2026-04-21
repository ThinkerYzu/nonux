# nonux - Composable Microkernel
# Top-level Makefile

CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

CFLAGS   := -ffreestanding -nostdlib -Wall -Wextra -Werror -O2 \
            -mno-outline-atomics -mgeneral-regs-only -mstrict-align \
            -I. -Igen
ASFLAGS  := -I.

QEMU     := qemu-system-aarch64
QEMU_MEM ?= 1G
QEMU_FLAGS := -M virt,gic-version=2 -cpu cortex-a53 -nographic -kernel kernel.bin -m $(QEMU_MEM)

# Core sources (always compiled)
CORE_S   := core/boot/start.S \
            core/cpu/vectors.S \
            core/cpu/context.S
CORE_C   := core/boot/boot.c \
            core/lib/string.c \
            core/lib/printf.c \
            core/lib/kheap.c \
            core/cpu/exception.c \
            core/pmm/pmm.c \
            core/irq/irq.c \
            core/irq/gic.c \
            core/timer/timer.c \
            core/sched/task.c \
            core/sched/sched.c

# Framework sources — compiled into every kernel build since slice 3.9a.
# Host tests compile these via test/host/Makefile with system cc instead.
FW_C     := framework/registry.c \
            framework/component.c \
            framework/hook.c \
            framework/ipc.c \
            framework/bootstrap.c

# Component sources + the gen/slot_table.c binding table, both emitted
# by `make kernel-config`.
-include gen/sources.mk
GEN_C    := gen/slot_table.c

ALL_S    := $(CORE_S)
ALL_C    := $(CORE_C) $(FW_C) $(COMPONENT_SRCS) $(GEN_C)

OBJS_S   := $(ALL_S:.S=.o)
OBJS_C   := $(ALL_C:.c=.o)
OBJS     := $(OBJS_S) $(OBJS_C)

# Default target
all: verify-registry kernel.bin

# Compile assembly
%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

# Compile C
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link
kernel.elf: $(OBJS) core/boot/linker.ld
	$(LD) -T core/boot/linker.ld -o $@ $(OBJS)

# Binary image
kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

# Run in QEMU
run: kernel.bin
	$(QEMU) $(QEMU_FLAGS)

# Debug with GDB
debug: kernel.bin
	$(QEMU) $(QEMU_FLAGS) -s -S &
	@echo "GDB server on :1234. Connect with:"
	@echo "  aarch64-linux-gnu-gdb kernel.elf -ex 'target remote :1234'"

# Python tooling — uses the project venv so jsonschema and other deps
# don't have to be installed system-wide. `make venv` below creates it.
VENV       ?= .venv
VENV_PY    := $(VENV)/bin/python3
VENV_STAMP := $(VENV)/.installed
PYTHON     ?= $(VENV_PY)

# Generator (no venv deps — stdlib only)
GENCONFIG := tools/gen-config.py
VALIDATE  := tools/validate-config.py
VERIFY    := tools/verify-registry.py

# Generated build artefacts from kernel.json.  Each output has a
# concrete rule so `make` can auto-run gen-config the first time
# anything depends on gen/sources.mk or gen/slot_table.c.  GNU make
# automatically re-execs after resolving an -include target.
#
# All three outputs are produced by a single gen-config invocation,
# so we cascade the other two off gen/sources.mk to avoid redundant
# regeneration.  kernel-config stays as a phony convenience target.
gen/sources.mk: kernel.json $(GENCONFIG)
	@mkdir -p gen
	$(PYTHON) $(GENCONFIG) kernel kernel.json components/ gen/

gen/config.h: gen/sources.mk
gen/slot_table.c: gen/sources.mk

kernel-config: gen/sources.mk
.PHONY: kernel-config

# Validate kernel config (needs venv → jsonschema)
validate-config: kernel.json $(VENV_STAMP)
	$(PYTHON) $(VALIDATE) kernel.json components/

# Dependency graph
deps: kernel.json $(VENV_STAMP)
	$(PYTHON) $(VALIDATE) kernel.json components/ --deps

deps-dot: kernel.json $(VENV_STAMP)
	$(PYTHON) $(VALIDATE) kernel.json components/ --deps-dot

# Static-checker build gate (DESIGN.md §AI Verification, R1-R8).
# verify-registry.py is stdlib-only today — R1/R3/R5/R6/R8 are marked
# `deferred` in the output; R2 and R4 are the currently-enforced
# regex-level checks. components/ is empty until Phase 4+, so this
# target is a no-op right now; wiring it as a prereq of kernel.bin
# and test now means it auto-kicks in when real components land.
verify-registry:
	$(PYTHON) $(VERIFY) components/
.PHONY: verify-registry

# One-shot venv setup: creates .venv/ and installs tools/requirements.txt.
# The stamp file $(VENV_STAMP) is our "tools are installed" sentinel —
# using `touch` on $(VENV_PY) directly is brittle because python3 is
# usually a symlink to a system-owned interpreter.
$(VENV_STAMP): tools/requirements.txt
	@test -x $(VENV_PY) || python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q -r tools/requirements.txt
	@touch $@

venv: $(VENV_STAMP)
.PHONY: venv

# In-kernel test binary (kernel-test.bin).  Same objects as kernel.bin
# except boot.o is replaced by boot-test.o (compiled with -DNX_KTEST so
# boot_main dispatches into ktest_main instead of the idle loop), plus
# the ktest runner and test cases.
KTEST_C       := test/kernel/ktest_main.c \
                 test/kernel/ktest_pmm.c \
                 test/kernel/ktest_irq.c \
                 test/kernel/ktest_bootstrap.c \
                 test/kernel/ktest_context.c \
                 test/kernel/ktest_sched_bootstrap.c
KTEST_OBJS    := $(KTEST_C:.c=.o)

core/boot/boot-test.o: core/boot/boot.c
	$(CC) $(CFLAGS) -DNX_KTEST -c $< -o $@

TEST_OBJS     := $(filter-out core/boot/boot.o,$(OBJS)) \
                 core/boot/boot-test.o \
                 $(KTEST_OBJS)

kernel-test.elf: $(TEST_OBJS) core/boot/linker.ld
	$(LD) -T core/boot/linker.ld -o $@ $(TEST_OBJS)

kernel-test.bin: kernel-test.elf
	$(OBJCOPY) -O binary $< $@

# Tests
test: verify-registry test-tools test-host test-kernel

test-host:
	$(MAKE) -C test/host

# Python tooling tests. Stdlib unittest + discover — no pytest dep.
# No venv needed unless jsonschema is imported; unittest skips those
# tests if the module is absent.
test-tools:
	$(PYTHON) -m unittest discover -s tools/tests -v

# Runs the in-kernel suite under QEMU with semihosting.  ktest_main
# exits via SYS_EXIT_EXTENDED, which QEMU propagates as its own exit
# code (0 = all tests passed).  An outer `timeout` catches hangs.
#
# QEMU writes the kernel's serial output to test/kernel-output.log; we
# `cat` it after QEMU exits.  This keeps QEMU completely off the parent
# TTY, which avoids a class of hangs observed under GNU screen/tmux
# where QEMU's -nographic or -serial stdio can deadlock with the
# multiplexer's pty handling.
KTEST_LOG := test/kernel-output.log

test-kernel: kernel-test.bin
	@rm -f $(KTEST_LOG)
	@-timeout --preserve-status 15 \
	    $(QEMU) -M virt,gic-version=2 -cpu cortex-a53 \
	    -display none -serial file:$(KTEST_LOG) -monitor none \
	    -m $(QEMU_MEM) -semihosting -kernel kernel-test.bin; \
	    rc=$$?; \
	    cat $(KTEST_LOG); \
	    exit $$rc

# Benchmarks
bench: kernel.bin
	timeout 60 $(QEMU) $(QEMU_FLAGS) -append "bench" | tee test/bench_output.log

# Clean
clean:
	find . -name '*.o' -delete
	rm -rf gen/ kernel.elf kernel.bin kernel-test.elf kernel-test.bin

.PHONY: all run debug validate-config deps deps-dot test test-host test-kernel test-tools bench clean
