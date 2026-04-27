# nonux - Composable Microkernel
# Top-level Makefile

CROSS    := aarch64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

CFLAGS   := -ffreestanding -nostdlib -Wall -Wextra -Werror -O2 \
            -mno-outline-atomics -mgeneral-regs-only \
            -I. -Igen
ASFLAGS  := -I.

QEMU     := qemu-system-aarch64
QEMU_MEM ?= 1G
QEMU_FLAGS := -M virt,gic-version=2 -cpu cortex-a53 -nographic -kernel kernel.bin -m $(QEMU_MEM)

# Core sources (always compiled)
CORE_S   := core/boot/start.S \
            core/cpu/vectors.S \
            core/cpu/context.S \
            core/cpu/el0_entry.S
CORE_C   := core/boot/boot.c \
            core/lib/string.c \
            core/lib/printf.c \
            core/lib/kheap.c \
            core/cpu/exception.c \
            core/mmu/mmu.c \
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
            framework/dispatcher.c \
            framework/bootstrap.c \
            framework/handle.c \
            framework/syscall.c \
            framework/channel.c \
            framework/console.c \
            framework/process.c \
            framework/elf.c

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
                 test/kernel/ktest_mmu.c \
                 test/kernel/ktest_pmm.c \
                 test/kernel/ktest_irq.c \
                 test/kernel/ktest_bootstrap.c \
                 test/kernel/ktest_context.c \
                 test/kernel/ktest_sched_bootstrap.c \
                 test/kernel/ktest_sched.c \
                 test/kernel/ktest_dispatcher.c \
                 test/kernel/ktest_mm_buddy.c \
                 test/kernel/ktest_handle.c \
                 test/kernel/ktest_syscall.c \
                 test/kernel/ktest_el0.c \
                 test/kernel/ktest_channel.c \
                 test/kernel/ktest_vfs.c \
                 test/kernel/ktest_process.c \
                 test/kernel/ktest_elf.c \
                 test/kernel/ktest_fork.c \
                 test/kernel/ktest_wait.c \
                 test/kernel/ktest_exec.c \
                 test/kernel/ktest_posix.c \
                 test/kernel/ktest_posix_pipe.c \
                 test/kernel/ktest_posix_signal.c \
                 test/kernel/ktest_posix_pipe_xproc.c \
                 test/kernel/ktest_initramfs.c \
                 test/kernel/ktest_posix_main.c \
                 test/kernel/ktest_posix_libc.c \
                 test/kernel/ktest_posix_printf.c \
                 test/kernel/ktest_argv_push.c \
                 test/kernel/ktest_posix_musl.c \
                 test/kernel/ktest_musl_exec.c \
                 test/kernel/ktest_posix_musl_printf.c \
                 test/kernel/ktest_posix_segfault.c \
                 test/kernel/ktest_posix_undef.c \
                 test/kernel/ktest_posix_busybox.c \
                 test/kernel/ktest_posix_busybox_sh.c \
                 test/kernel/ktest_posix_busybox_sh_echo.c \
                 test/kernel/ktest_posix_busybox_sh_echo_seq.c \
                 test/kernel/ktest_posix_busybox_sh_ls.c \
                 test/kernel/ktest_posix_busybox_sh_pipe.c \
                 test/kernel/ktest_posix_busybox_sh_cat.c \
                 test/kernel/ktest_posix_busybox_sh_redir.c

# EL0 test programs assembled into kernel-test.bin's .rodata — each
# is memcpy'd into the MMU's user window by its matching ktest before
# drop_to_el0.  Not part of kernel.bin; test-only scaffold.
KTEST_S       := test/kernel/user_prog.S \
                 test/kernel/user_prog_chan.S \
                 test/kernel/user_prog_file.S \
                 test/kernel/user_prog_readdir.S \
                 test/kernel/user_prog_fork.S \
                 test/kernel/user_prog_wait.S \
                 test/kernel/user_prog_exec.S \
                 test/kernel/init_prog_blob.S \
                 test/kernel/posix_prog_blob.S \
                 test/kernel/posix_pipe_prog_blob.S \
                 test/kernel/posix_signal_prog_blob.S \
                 test/kernel/posix_pipe_xproc_prog_blob.S \
                 test/kernel/initramfs_blob.S \
                 test/kernel/posix_main_prog_blob.S \
                 test/kernel/posix_libc_prog_blob.S \
                 test/kernel/posix_printf_prog_blob.S \
                 test/kernel/argv_parent_prog_blob.S \
                 test/kernel/posix_musl_prog_blob.S \
                 test/kernel/musl_exec_parent_prog_blob.S \
                 test/kernel/posix_musl_printf_prog_blob.S \
                 test/kernel/posix_segfault_prog_blob.S \
                 test/kernel/posix_undef_prog_blob.S \
                 test/kernel/posix_busybox_help_prog_blob.S \
                 test/kernel/posix_busybox_sh_prog_blob.S \
                 test/kernel/posix_busybox_sh_echo_prog_blob.S \
                 test/kernel/posix_busybox_sh_echo_seq_prog_blob.S \
                 test/kernel/posix_busybox_sh_ls_prog_blob.S \
                 test/kernel/posix_busybox_sh_pipe_prog_blob.S \
                 test/kernel/posix_busybox_sh_cat_prog_blob.S \
                 test/kernel/posix_busybox_sh_redir_prog_blob.S

# Slice 7.3: a tiny standalone EL0 ELF linked at the user-window VA.
# Built as its own aarch64 executable, then embedded into kernel-test.bin
# via `.incbin` inside `init_prog_blob.S`.  `framework/elf.c` parses
# the blob at test time and loads its PT_LOAD segments into a target
# process's address space.
test/kernel/init_prog.o: test/kernel/init_prog.S
	$(CC) $(ASFLAGS) -c $< -o $@

# -n (--nmagic) turns off page alignment so the LOAD segment starts
# right after the header + phdr table rather than at file offset
# 0x10000 (default page align).  Drops the embedded blob from ~66 KiB
# to ~150 bytes — matters for slice 7.4c, which seeds ramfs with the
# ELF via the vfs syscalls (ramfs files cap at RAMFS_FILE_CAP bytes).
test/kernel/init_prog.elf: test/kernel/init_prog.o test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ test/kernel/init_prog.o

# .incbin brings the ELF bytes into .rodata; assembler doesn't track
# .incbin dependencies automatically, so add an explicit one.
test/kernel/init_prog_blob.o: test/kernel/init_prog_blob.S test/kernel/init_prog.elf

# Slice 7.4d: a C-compiled EL0 ELF that exercises the posix_shim
# header-only wrappers.  Reuses init_prog.ld for the user-window
# VA.  -ffreestanding -nostdlib keeps the build independent of any
# libc; -mgeneral-regs-only / -mno-outline-atomics mirror the
# kernel's flags so we produce pure integer EL0 code.
POSIX_PROG_CFLAGS := -ffreestanding -nostdlib -Wall -Wextra -Werror -O2 \
                     -mno-outline-atomics -mgeneral-regs-only \
                     -fno-stack-protector -fno-pic -I.
test/kernel/posix_prog.o: test/kernel/posix_prog.c components/posix_shim/posix.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_prog.elf: test/kernel/posix_prog.o test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ test/kernel/posix_prog.o

test/kernel/posix_prog_blob.o: test/kernel/posix_prog_blob.S test/kernel/posix_prog.elf

# Slice 7.5 pipe demo — same flag / linker-script recipe as
# posix_prog.elf.  Single-process write→read roundtrip through the
# NX_SYS_PIPE + type-polymorphic NX_SYS_READ/WRITE path.
test/kernel/posix_pipe_prog.o: test/kernel/posix_pipe_prog.c components/posix_shim/posix.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_pipe_prog.elf: test/kernel/posix_pipe_prog.o test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ test/kernel/posix_pipe_prog.o

test/kernel/posix_pipe_prog_blob.o: test/kernel/posix_pipe_prog_blob.S test/kernel/posix_pipe_prog.elf

# Slice 7.5 signal demo — parent fork + SIGTERM to child.
test/kernel/posix_signal_prog.o: test/kernel/posix_signal_prog.c components/posix_shim/posix.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_signal_prog.elf: test/kernel/posix_signal_prog.o test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ test/kernel/posix_signal_prog.o

test/kernel/posix_signal_prog_blob.o: test/kernel/posix_signal_prog_blob.S test/kernel/posix_signal_prog.elf

# Slice 7.6 prereq — cross-process pipe demo.  Same flag set + linker
# script as posix_prog.elf; just a different .c source.  Validates
# fork's handle-table inheritance for HANDLE_CHANNEL endpoints.
test/kernel/posix_pipe_xproc_prog.o: test/kernel/posix_pipe_xproc_prog.c components/posix_shim/posix.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_pipe_xproc_prog.elf: test/kernel/posix_pipe_xproc_prog.o test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ test/kernel/posix_pipe_xproc_prog.o

test/kernel/posix_pipe_xproc_prog_blob.o: test/kernel/posix_pipe_xproc_prog_blob.S test/kernel/posix_pipe_xproc_prog.elf

# Slice 7.6b — initramfs.  `tools/pack-initramfs.py` packs a list of
# entries into a cpio-newc archive; the resulting blob is embedded
# into kernel-test.bin's .rodata via `.incbin` and surfaced through
# the weak `__ramfs_initramfs_blob_{start,end}` symbols.  ramfs's
# init() walks the blob and seeds itself.  Stays test-only (production
# `kernel.bin` doesn't link `initramfs_blob.S` and so its weak symbols
# stay 0, skipping the slurp).
#
# Manifest of test files:
#   /init    — init_prog.elf (the slice-7.3 EL0 binary; lets ktest_exec
#              stop hand-seeding via vfs syscalls).
#   /banner  — a tiny ASCII payload that ktest_initramfs reads back.
#              Deliberately not `/hello` — ktest_vfs already creates
#              that file via vfs_simple's create flow, and we don't
#              want a registration-order race between the slurp and
#              that test.
test/kernel/banner.txt:
	@printf 'hello from initramfs\n' > $@

test/kernel/initramfs.cpio: tools/pack-initramfs.py \
                            test/kernel/init_prog.elf \
                            test/kernel/banner.txt \
                            test/kernel/argv_child_prog.elf \
                            test/kernel/posix_musl_prog.elf \
                            $(BUSYBOX_BIN)
	$(PYTHON) tools/pack-initramfs.py $@ \
	    test/kernel/init_prog.elf:/init \
	    test/kernel/banner.txt:/banner \
	    test/kernel/argv_child_prog.elf:/argv_child \
	    test/kernel/posix_musl_prog.elf:/musl_prog \
	    $(BUSYBOX_BIN):/bin/busybox \
	    $(BUSYBOX_BIN):/bin/ls \
	    $(BUSYBOX_BIN):/bin/cat \
	    $(BUSYBOX_BIN):/bin/echo

test/kernel/initramfs_blob.o: test/kernel/initramfs_blob.S \
                              test/kernel/initramfs.cpio

# Slice 7.6c.0 — EL0 C-runtime bootstrap.  posix_shim's crt0.S owns
# the ELF entry symbol `_start` and provides the standard POSIX
# transition: argc/argv/envp setup, `bl main`, `nx_posix_exit(rv)`.
# Linked first so `_start` lands at the user-window base.
components/posix_shim/crt0.o: components/posix_shim/crt0.S
	$(CC) $(ASFLAGS) -c $< -o $@

# C demo using `int main(int argc, char **argv, char **envp)` entry
# (instead of the bare `_start` shape every other slice-7.x EL0
# program uses).  Build flags identical to posix_prog.elf — same
# linker script, same freestanding stance.  Linker order matters:
# crt0.o first so `_start` is at offset 0 in the .text section.
test/kernel/posix_main_prog.o: test/kernel/posix_main_prog.c \
                               components/posix_shim/posix.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_main_prog.elf: components/posix_shim/crt0.o \
                                 test/kernel/posix_main_prog.o \
                                 test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    components/posix_shim/crt0.o test/kernel/posix_main_prog.o

test/kernel/posix_main_prog_blob.o: test/kernel/posix_main_prog_blob.S \
                                    test/kernel/posix_main_prog.elf

# Slice 7.6c.1 — libnxlibc.a: the POSIX-named C surface packaged as
# a static archive.  EL0 C programs link with `-lnxlibc` to pull in
# crt0 + the lowercase POSIX wrappers (`write`, `_exit`, `strlen`,
# ...).  Slice 7.6c.2's musl pin will replace this archive with
# musl's `libc.a`; programs don't need to change source.
AR := $(CROSS)ar
components/posix_shim/nxlibc.o: components/posix_shim/nxlibc.c \
                                components/posix_shim/posix.h \
                                components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

components/posix_shim/libnxlibc.a: components/posix_shim/crt0.o \
                                   components/posix_shim/nxlibc.o
	$(AR) rcs $@ components/posix_shim/crt0.o components/posix_shim/nxlibc.o

# C demo that links against libnxlibc.a.  The linker pulls `crt0.o`
# out of the archive (it's referenced through `_start` — the ELF
# entry set by `init_prog.ld`) and `nxlibc.o` through the
# POSIX-named symbols the demo calls.  `-L` + `-l` is the standard
# GNU ld archive-search incantation.
test/kernel/posix_libc_prog.o: test/kernel/posix_libc_prog.c \
                               components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_libc_prog.elf: test/kernel/posix_libc_prog.o \
                                 components/posix_shim/libnxlibc.a \
                                 test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_libc_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_libc_prog_blob.o: test/kernel/posix_libc_prog_blob.S \
                                    test/kernel/posix_libc_prog.elf

# Slice 7.6c.2 — printf demo.  Same flag set + linker shape as
# posix_libc_prog; just exercises printf / atoi / puts instead.
test/kernel/posix_printf_prog.o: test/kernel/posix_printf_prog.c \
                                 components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_printf_prog.elf: test/kernel/posix_printf_prog.o \
                                   components/posix_shim/libnxlibc.a \
                                   test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_printf_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_printf_prog_blob.o: test/kernel/posix_printf_prog_blob.S \
                                      test/kernel/posix_printf_prog.elf

# Slice 7.6c.4 — argv-push round-trip.  Two C-compiled EL0 binaries:
#   argv_child_prog.elf  — exec'd target, validates argc/argv content,
#                          shipped in initramfs as `/argv_child` so
#                          sys_exec can load it via vfs.
#   argv_parent_prog.elf — drops to EL0 directly; forks + execve's
#                          /argv_child with explicit argv, waits.
test/kernel/argv_child_prog.o: test/kernel/argv_child_prog.c \
                               components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/argv_child_prog.elf: test/kernel/argv_child_prog.o \
                                 components/posix_shim/libnxlibc.a \
                                 test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/argv_child_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/argv_parent_prog.o: test/kernel/argv_parent_prog.c \
                                components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/argv_parent_prog.elf: test/kernel/argv_parent_prog.o \
                                  components/posix_shim/libnxlibc.a \
                                  test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/argv_parent_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/argv_parent_prog_blob.o: test/kernel/argv_parent_prog_blob.S \
                                     test/kernel/argv_parent_prog.elf

# Slice 7.6c.3c — sys_exec → musl-linked-child round-trip.  Parent
# is libnxlibc-linked (same shape as argv_parent_prog); execs
# /musl_prog (the slice 7.6c.3b posix_musl_prog.elf, ramfs-seeded
# under that path).  Validates AUXV consumption end-to-end —
# slice 7.6c.3b's AUXV push only fires through sys_exec, and only
# this test actually drives a sys_exec → musl-linked image.
test/kernel/musl_exec_parent_prog.o: test/kernel/musl_exec_parent_prog.c \
                                     components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/musl_exec_parent_prog.elf: test/kernel/musl_exec_parent_prog.o \
                                       components/posix_shim/libnxlibc.a \
                                       test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/musl_exec_parent_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/musl_exec_parent_prog_blob.o: test/kernel/musl_exec_parent_prog_blob.S \
                                          test/kernel/musl_exec_parent_prog.elf

# Slice 7.6d.2c — first attempt at exec'ing busybox.  libnxlibc-linked
# parent forks + execve("/bin/busybox", { ..., "--help", NULL }, NULL)
# against the initramfs-seeded busybox binary.  Same link recipe as
# musl_exec_parent — small libnxlibc-linked C program, no musl.
test/kernel/posix_busybox_help_prog.o: test/kernel/posix_busybox_help_prog.c \
                                       components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_help_prog.elf: test/kernel/posix_busybox_help_prog.o \
                                         components/posix_shim/libnxlibc.a \
                                         test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_help_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_help_prog_blob.o: test/kernel/posix_busybox_help_prog_blob.S \
                                            test/kernel/posix_busybox_help_prog.elf

# Slice 7.6d.N.0 — first attempt at exec'ing busybox AS A SHELL.
# Same recipe as posix_busybox_help_prog but the program drives
# busybox via { "sh", "-c", "exit 42", NULL } so basename(argv[0])
# routes to the ash applet (CONFIG_SH_IS_ASH=y).
test/kernel/posix_busybox_sh_prog.o: test/kernel/posix_busybox_sh_prog.c \
                                     components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_prog.elf: test/kernel/posix_busybox_sh_prog.o \
                                       components/posix_shim/libnxlibc.a \
                                       test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_prog_blob.o: test/kernel/posix_busybox_sh_prog_blob.S \
                                          test/kernel/posix_busybox_sh_prog.elf

# Slice 7.6d.N.2 — busybox `sh -c "echo hello"` discovery.  Same
# recipe as posix_busybox_sh_prog; only the embedded -c string
# differs.  Discovery-driven escalation past the slice-7.6d.N.1
# `exit 42` baseline.
test/kernel/posix_busybox_sh_echo_prog.o: test/kernel/posix_busybox_sh_echo_prog.c \
                                          components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_echo_prog.elf: test/kernel/posix_busybox_sh_echo_prog.o \
                                            components/posix_shim/libnxlibc.a \
                                            test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_echo_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_echo_prog_blob.o: test/kernel/posix_busybox_sh_echo_prog_blob.S \
                                               test/kernel/posix_busybox_sh_echo_prog.elf

# Slice 7.6d.N.3 — busybox `sh -c "echo a; echo b"` discovery.
# Same recipe as posix_busybox_sh_echo_prog; only the embedded
# -c string differs (multi-statement script via `;` separator).
# Discovery-driven escalation past slice 7.6d.N.2's single-
# statement `echo hello` baseline.
test/kernel/posix_busybox_sh_echo_seq_prog.o: test/kernel/posix_busybox_sh_echo_seq_prog.c \
                                              components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_echo_seq_prog.elf: test/kernel/posix_busybox_sh_echo_seq_prog.o \
                                                components/posix_shim/libnxlibc.a \
                                                test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_echo_seq_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_echo_seq_prog_blob.o: test/kernel/posix_busybox_sh_echo_seq_prog_blob.S \
                                                   test/kernel/posix_busybox_sh_echo_seq_prog.elf

# Slice 7.6d.N.4 — busybox `sh -c "ls /"` discovery.  First
# non-builtin escalation: ash forks + execve's `/bin/ls` (a
# duplicate cpio entry pointing at the same busybox blob; busybox
# dispatches to the `ls` applet via `basename(argv[0])`).
test/kernel/posix_busybox_sh_ls_prog.o: test/kernel/posix_busybox_sh_ls_prog.c \
                                        components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_ls_prog.elf: test/kernel/posix_busybox_sh_ls_prog.o \
                                          components/posix_shim/libnxlibc.a \
                                          test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_ls_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_ls_prog_blob.o: test/kernel/posix_busybox_sh_ls_prog_blob.S \
                                             test/kernel/posix_busybox_sh_ls_prog.elf

# Slice 7.6d.N.6 — busybox `sh -c "echo hello | cat"` discovery.
# First pipe escalation: ash forks twice (one process per pipeline
# stage), wires them with pipe(2) + dup2.  Will likely surface
# unmapped __NR_pipe2 / __NR_dup3 / __NR_readv before it runs.
test/kernel/posix_busybox_sh_pipe_prog.o: test/kernel/posix_busybox_sh_pipe_prog.c \
                                          components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_pipe_prog.elf: test/kernel/posix_busybox_sh_pipe_prog.o \
                                            components/posix_shim/libnxlibc.a \
                                            test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_pipe_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_pipe_prog_blob.o: test/kernel/posix_busybox_sh_pipe_prog_blob.S \
                                               test/kernel/posix_busybox_sh_pipe_prog.elf

# Slice 7.6d.N.7 — busybox `sh -c "cat /banner"`.  First file-input
# escalation past the pipe slice: ash forks once, child execve's
# busybox-as-cat against the /banner ramfs file.  Drives cat reading
# from a FILE handle (vs the CHANNEL handle exercised by the pipe
# slice).
test/kernel/posix_busybox_sh_cat_prog.o: test/kernel/posix_busybox_sh_cat_prog.c \
                                         components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_cat_prog.elf: test/kernel/posix_busybox_sh_cat_prog.o \
                                           components/posix_shim/libnxlibc.a \
                                           test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_cat_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_cat_prog_blob.o: test/kernel/posix_busybox_sh_cat_prog_blob.S \
                                              test/kernel/posix_busybox_sh_cat_prog.elf

# Slice 7.6d.N.8 — busybox `sh -c "echo a > /tmp/foo"`.  First stdout-
# redirection-to-file escalation past the cat slice.  Same recipe as
# posix_busybox_sh_cat_prog; only the embedded -c string differs.
test/kernel/posix_busybox_sh_redir_prog.o: test/kernel/posix_busybox_sh_redir_prog.c \
                                           components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_busybox_sh_redir_prog.elf: test/kernel/posix_busybox_sh_redir_prog.o \
                                             components/posix_shim/libnxlibc.a \
                                             test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_busybox_sh_redir_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_busybox_sh_redir_prog_blob.o: test/kernel/posix_busybox_sh_redir_prog_blob.S \
                                                test/kernel/posix_busybox_sh_redir_prog.elf

# Slice 7.6d.3a — EL0-fault demos.  Each is a libnxlibc-linked C
# program: parent forks; child trips a fault (NULL write for the
# segfault demo, `udf #0` for the undef demo); parent waits and
# observes 128+SIGSEGV / 128+SIGILL via the new `on_sync` fault-
# conversion path.
test/kernel/posix_segfault_prog.o: test/kernel/posix_segfault_prog.c \
                                   components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_segfault_prog.elf: test/kernel/posix_segfault_prog.o \
                                     components/posix_shim/libnxlibc.a \
                                     test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_segfault_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_segfault_prog_blob.o: test/kernel/posix_segfault_prog_blob.S \
                                        test/kernel/posix_segfault_prog.elf

test/kernel/posix_undef_prog.o: test/kernel/posix_undef_prog.c \
                                components/posix_shim/nxlibc.h
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_undef_prog.elf: test/kernel/posix_undef_prog.o \
                                  components/posix_shim/libnxlibc.a \
                                  test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    test/kernel/posix_undef_prog.o \
	    -Lcomponents/posix_shim -lnxlibc

test/kernel/posix_undef_prog_blob.o: test/kernel/posix_undef_prog_blob.S \
                                     test/kernel/posix_undef_prog.elf

# Slice 7.6c.3c — musl-linked printf demo.  Pulls in musl's
# vfprintf -> long-double softfloat helpers (__netf2, __fixtfsi,
# __addtf3, ...) which live in libgcc.a, not musl.  `LIBGCC` is
# the cross-compiler's libgcc path; --start-group/--end-group
# resolves the cycle between libc.a and libgcc.a.
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

test/kernel/posix_musl_printf_prog.o: test/kernel/posix_musl_printf_prog.c
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_musl_printf_prog.elf: test/kernel/posix_musl_printf_prog.o \
                                        $(MUSL_CRT1) $(MUSL_CRTI) \
                                        $(MUSL_LIBC) $(MUSL_CRTN) \
                                        test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    $(MUSL_CRT1) $(MUSL_CRTI) \
	    test/kernel/posix_musl_printf_prog.o \
	    --start-group $(MUSL_LIBC) $(LIBGCC) --end-group \
	    $(MUSL_CRTN)

test/kernel/posix_musl_printf_prog_blob.o: test/kernel/posix_musl_printf_prog_blob.S \
                                           test/kernel/posix_musl_printf_prog.elf

KTEST_S_OBJS  := $(KTEST_S:.S=.o)
KTEST_OBJS    := $(KTEST_C:.c=.o)

core/boot/boot-test.o: core/boot/boot.c
	$(CC) $(CFLAGS) -DNX_KTEST -c $< -o $@

TEST_OBJS     := $(filter-out core/boot/boot.o,$(OBJS)) \
                 core/boot/boot-test.o \
                 $(KTEST_OBJS) \
                 $(KTEST_S_OBJS)

kernel-test.elf: $(TEST_OBJS) core/boot/linker.ld
	$(LD) -T core/boot/linker.ld -o $@ $(TEST_OBJS)

kernel-test.bin: kernel-test.elf
	$(OBJCOPY) -O binary $< $@

# Slice 7.6c.3a/b — vendored musl libc.a + crt objects.  Source tree
# lives at third_party/musl/ (musl 1.2.5 snapshot, MIT-clean upstream
# + nonux patches in arch/aarch64/syscall_arch.h and
# src/thread/aarch64/syscall_cp.s — both translate Linux-aarch64
# syscall numbers into our NX_SYS_* numbers before `svc 0`).
#
# The build runs musl's own ./configure once (stamped by .configured),
# then `make lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o` inside the
# musl tree.  Slice 7.6c.3b adds the crt objects so EL0 C programs
# can link with musl's standard `-lc + crt1 + crti/crtn` shape.
MUSL_DIR    := third_party/musl
MUSL_LIBC   := $(MUSL_DIR)/lib/libc.a
MUSL_CRT1   := $(MUSL_DIR)/lib/crt1.o
MUSL_CRTI   := $(MUSL_DIR)/lib/crti.o
MUSL_CRTN   := $(MUSL_DIR)/lib/crtn.o
MUSL_STAMP  := $(MUSL_DIR)/.configured

$(MUSL_STAMP):
	cd $(MUSL_DIR) && ./configure \
	    --target=aarch64-linux-gnu \
	    --prefix=/usr/local/musl \
	    --disable-shared \
	    CC=$(CC) CROSS_COMPILE=$(CROSS) >/dev/null
	@touch $@

$(MUSL_LIBC) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN): \
              $(MUSL_STAMP) \
              $(MUSL_DIR)/arch/aarch64/syscall_arch.h \
              $(MUSL_DIR)/src/thread/aarch64/syscall_cp.s
	@# musl's internal Makefile tracks .c -> .lo deps but NOT
	@# arch-header inclusion; any patch to syscall_arch.h leaves the
	@# .lo cache stale.  Wipe obj/ + lib/ when our deps trigger so the
	@# patched translation lands.  Costs ~30 s on patch; no-op on
	@# everything else (the rule only re-fires when its deps change).
	rm -rf $(MUSL_DIR)/obj $(MUSL_DIR)/lib
	$(MAKE) -C $(MUSL_DIR) lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o

musl-libc: $(MUSL_LIBC) $(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN)
.PHONY: musl-libc

musl-clean:
	-$(MAKE) -C $(MUSL_DIR) clean 2>/dev/null
	rm -f $(MUSL_STAMP) $(MUSL_DIR)/config.mak
	rm -rf $(MUSL_DIR)/_sysroot
.PHONY: musl-clean

# Slice 7.6d.1 — cross-compile busybox against our patched musl.
#
# Build infrastructure only.  No boot integration yet (that's 7.6d.N's
# job).  busybox source is vendored under third_party/busybox/ from
# upstream's 1.36.1 tarball; our minimal config is committed at
# third_party/busybox/configs/nonux_defconfig.  tools/build-busybox.sh
# stages the config + drives the build against a private musl sysroot
# at third_party/musl/_sysroot/ (populated lazily — install-headers
# for include/, symlinks back to lib/ for the libs).
#
# busybox is intentionally NOT a dep of `make test` yet; the build
# takes ~30 s and the artefact isn't consumed by any ktest in 7.6d.1.
# It becomes a test dep when 7.6d.2 first execs it from a libnxlibc
# parent.
BUSYBOX_DIR    := third_party/busybox
BUSYBOX_BIN    := $(BUSYBOX_DIR)/busybox
BUSYBOX_CONFIG := $(BUSYBOX_DIR)/configs/nonux_defconfig
BUSYBOX_BUILD  := tools/build-busybox.sh

$(BUSYBOX_BIN): $(BUSYBOX_BUILD) $(BUSYBOX_CONFIG) $(MUSL_LIBC)
	CROSS=$(CROSS) $(BUSYBOX_BUILD)

busybox: $(BUSYBOX_BIN)
.PHONY: busybox

busybox-clean:
	-$(MAKE) -C $(BUSYBOX_DIR) clean 2>/dev/null
	rm -f $(BUSYBOX_DIR)/.config
.PHONY: busybox-clean

# Slice 7.6c.3b — first EL0 C demo against musl's libc.a + crt set.
# Same freestanding flag set as the libnxlibc-linked demos; link line
# differs only in the libc + crt selection.  -nostdlib keeps gcc's
# default crt0/crt1 search out of the way; we explicitly list
# musl's crt1.o + crti.o (program-init prologue) + libc.a + crtn.o
# (program-init epilogue).
test/kernel/posix_musl_prog.o: test/kernel/posix_musl_prog.c
	$(CC) $(POSIX_PROG_CFLAGS) -c $< -o $@

test/kernel/posix_musl_prog.elf: test/kernel/posix_musl_prog.o \
                                 $(MUSL_CRT1) $(MUSL_CRTI) \
                                 $(MUSL_LIBC) $(MUSL_CRTN) \
                                 test/kernel/init_prog.ld
	$(LD) -n -T test/kernel/init_prog.ld -o $@ \
	    $(MUSL_CRT1) $(MUSL_CRTI) \
	    test/kernel/posix_musl_prog.o \
	    --start-group $(MUSL_LIBC) --end-group \
	    $(MUSL_CRTN)

test/kernel/posix_musl_prog_blob.o: test/kernel/posix_musl_prog_blob.S \
                                    test/kernel/posix_musl_prog.elf

# Tests
test: verify-registry test-tools test-host test-kernel musl-libc

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
	@-timeout --preserve-status 90 \
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
	rm -rf gen/ kernel.elf kernel.bin kernel-test.elf kernel-test.bin \
	       test/kernel/init_prog.elf test/kernel/posix_prog.elf \
	       test/kernel/posix_pipe_prog.elf test/kernel/posix_signal_prog.elf \
	       test/kernel/posix_pipe_xproc_prog.elf \
	       test/kernel/posix_main_prog.elf \
	       test/kernel/posix_libc_prog.elf \
	       test/kernel/posix_printf_prog.elf \
	       test/kernel/argv_parent_prog.elf \
	       test/kernel/argv_child_prog.elf \
	       test/kernel/posix_musl_prog.elf \
	       test/kernel/musl_exec_parent_prog.elf \
	       test/kernel/posix_musl_printf_prog.elf \
	       test/kernel/posix_busybox_help_prog.elf \
	       test/kernel/posix_busybox_sh_prog.elf \
	       test/kernel/posix_busybox_sh_echo_prog.elf \
	       test/kernel/posix_busybox_sh_echo_seq_prog.elf \
	       test/kernel/posix_busybox_sh_ls_prog.elf \
	       test/kernel/posix_busybox_sh_pipe_prog.elf \
	       test/kernel/posix_busybox_sh_cat_prog.elf \
	       test/kernel/posix_busybox_sh_redir_prog.elf \
	       test/kernel/posix_segfault_prog.elf \
	       test/kernel/posix_undef_prog.elf \
	       components/posix_shim/libnxlibc.a \
	       test/kernel/initramfs.cpio test/kernel/banner.txt

.PHONY: all run debug validate-config deps deps-dot test test-host test-kernel test-tools bench clean
