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

# Tests
test: test-host test-kernel

test-host:
	$(MAKE) -C test/host

test-kernel: kernel.bin
	timeout 30 $(QEMU) $(QEMU_FLAGS) -append "test" | tee test/output.log
	python3 test/check_results.py test/output.log

# Benchmarks
bench: kernel.bin
	timeout 60 $(QEMU) $(QEMU_FLAGS) -append "bench" | tee test/bench_output.log

# Clean
clean:
	find . -name '*.o' -delete
	rm -rf gen/ kernel.elf kernel.bin

.PHONY: all run debug validate-config deps deps-dot test test-host test-kernel bench clean
