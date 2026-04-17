# nonux - Composable Microkernel
# Top-level Makefile

CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

CFLAGS   := -ffreestanding -nostdlib -Wall -Wextra -Werror -O2 \
            -Icore -Iframework -Iinterfaces -Igen
ASFLAGS  := -Icore

QEMU     := qemu-system-aarch64
QEMU_MEM ?= 1G
QEMU_FLAGS := -M virt -cpu cortex-a53 -nographic -kernel kernel.bin -m $(QEMU_MEM)

# Core sources (always compiled)
CORE_S   := $(wildcard core/boot/*.S core/cpu/*.S)
CORE_C   := $(wildcard core/boot/*.c core/cpu/*.c core/pmm/*.c core/irq/*.c core/lib/*.c)
FW_C     := $(wildcard framework/*.c)

# Component sources (generated from kernel.json)
-include gen/sources.mk

SRCS     := $(CORE_S) $(CORE_C) $(FW_C) $(COMPONENT_SRCS)
OBJS     := $(SRCS:.S=.o)
OBJS     := $(OBJS:.c=.o)

# Default target
all: kernel.bin

# Generate config from kernel.json
gen/config.h gen/sources.mk: kernel.json tools/gen-config.py
	@mkdir -p gen
	python3 tools/gen-config.py kernel.json gen/

# Compile
%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

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
