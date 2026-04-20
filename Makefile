# nonux - Composable Microkernel
# Top-level Makefile

CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

CFLAGS   := -ffreestanding -nostdlib -Wall -Wextra -Werror -O2 \
            -mno-outline-atomics \
            -I. -Igen
ASFLAGS  := -I.

QEMU     := qemu-system-aarch64
QEMU_MEM ?= 1G
QEMU_FLAGS := -M virt,gic-version=2 -cpu cortex-a53 -nographic -kernel kernel.bin -m $(QEMU_MEM)

# Core sources (always compiled)
CORE_S   := core/boot/start.S \
            core/cpu/vectors.S
CORE_C   := core/boot/boot.c \
            core/lib/string.c \
            core/lib/printf.c \
            core/cpu/exception.c \
            core/pmm/pmm.c \
            core/irq/irq.c \
            core/irq/gic.c \
            core/timer/timer.c

# Framework sources (added as they're written)
FW_C     :=

# Component sources (generated from kernel.json in future phases)
-include gen/sources.mk

ALL_S    := $(CORE_S)
ALL_C    := $(CORE_C) $(FW_C) $(COMPONENT_SRCS)

OBJS_S   := $(ALL_S:.S=.o)
OBJS_C   := $(ALL_C:.c=.o)
OBJS     := $(OBJS_S) $(OBJS_C)

# Default target
all: kernel.bin

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

# Validate kernel config
validate-config: kernel.json
	python3 tools/validate-config.py kernel.json components/

# Dependency graph
deps: kernel.json
	python3 tools/validate-config.py kernel.json components/ --deps

deps-dot: kernel.json
	python3 tools/validate-config.py kernel.json components/ --deps-dot

# In-kernel test binary (kernel-test.bin).  Same objects as kernel.bin
# except boot.o is replaced by boot-test.o (compiled with -DNX_KTEST so
# boot_main dispatches into ktest_main instead of the idle loop), plus
# the ktest runner and test cases.
KTEST_C       := test/kernel/ktest_main.c \
                 test/kernel/ktest_pmm.c \
                 test/kernel/ktest_irq.c
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
test: test-host test-kernel

test-host:
	$(MAKE) -C test/host

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

.PHONY: all run debug validate-config deps deps-dot test test-host test-kernel bench clean
