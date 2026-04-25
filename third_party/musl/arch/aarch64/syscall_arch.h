/*
 * nonux patch — slice 7.6c.3a.
 *
 * The stock file emits `svc 0` with the Linux-aarch64 syscall number
 * in x8.  Our kernel uses a different (smaller) set of NX_SYS_*
 * numbers; this patch interposes a translation table at the SVC site
 * so musl's `__syscall*` family lands on the right kernel handler.
 *
 * The table is __nx_translate() below.  When `n` is a compile-time
 * constant (every `__syscall(__NR_foo, ...)` call site is), the
 * switch + early-return collapses under -O2 to either a single mov
 * (mapped) or a const `-ENOSYS` (unmapped).  No runtime overhead vs
 * the stock svc emit.
 *
 * Number-only mapping; argument layouts must already match.  Mapped
 * syscalls today:
 *   __NR_close       (57)  -> NX_SYS_HANDLE_CLOSE  (2)
 *   __NR_lseek       (62)  -> NX_SYS_SEEK          (9)
 *   __NR_read        (63)  -> NX_SYS_READ          (7)
 *   __NR_write       (64)  -> NX_SYS_WRITE         (8)
 *   __NR_exit        (93)  -> NX_SYS_EXIT          (11)
 *   __NR_exit_group  (94)  -> NX_SYS_EXIT          (11)   [v1 alias]
 *   __NR_kill       (129)  -> NX_SYS_SIGNAL        (16)
 *   __NR_execve     (221)  -> NX_SYS_EXEC          (14)
 *   __NR_wait4      (260)  -> NX_SYS_WAIT          (13)   [drops options+rusage]
 *
 * Unmapped syscalls (and everything else) return -ENOSYS (-38) so
 * musl's wrappers translate to errno=ENOSYS at the call site.  Slice
 * 7.6c.3b/c will tackle the harder cases — openat (needs dirfd
 * stripping), pipe2 (drops flags), clone (only the SIGCHLD-only fork
 * shape), brk/mmap (needs kernel-side allocator), clock_gettime
 * (needs kernel-side timekeeping plumbing), set_tid_address (needs a
 * tid concept).  The translation table is the only thing that
 * changes; all other musl source stays vanilla.
 */

static inline long __nx_translate(long n)
{
	switch (n) {
	case 57:  return 2;   /* __NR_close       -> NX_SYS_HANDLE_CLOSE */
	case 62:  return 9;   /* __NR_lseek       -> NX_SYS_SEEK */
	case 63:  return 7;   /* __NR_read        -> NX_SYS_READ */
	case 64:  return 8;   /* __NR_write       -> NX_SYS_WRITE */
	case 93:  return 11;  /* __NR_exit        -> NX_SYS_EXIT */
	case 94:  return 11;  /* __NR_exit_group  -> NX_SYS_EXIT */
	case 129: return 16;  /* __NR_kill        -> NX_SYS_SIGNAL */
	case 221: return 14;  /* __NR_execve      -> NX_SYS_EXEC */
	case 260: return 13;  /* __NR_wait4       -> NX_SYS_WAIT */
	default:  return -1;
	}
}

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define __asm_syscall(...) do { \
	__asm__ __volatile__ ( "svc 0" \
	: "=r"(x0) : __VA_ARGS__ : "memory", "cc"); \
	return x0; \
	} while (0)

static inline long __syscall0(long n)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0");
	__asm_syscall("r"(x8));
}

static inline long __syscall1(long n, long a)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	__asm_syscall("r"(x8), "0"(x0));
}

static inline long __syscall2(long n, long a, long b)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	__asm_syscall("r"(x8), "0"(x0), "r"(x1));
}

static inline long __syscall3(long n, long a, long b, long c)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm_syscall("r"(x8), "0"(x0), "r"(x1), "r"(x2));
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm_syscall("r"(x8), "0"(x0), "r"(x1), "r"(x2), "r"(x3));
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	__asm_syscall("r"(x8), "0"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4));
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	long __nx_n = __nx_translate(n);
	if (__nx_n < 0) return -38;
	register long x8 __asm__("x8") = __nx_n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm_syscall("r"(x8), "0"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5));
}

#define VDSO_USEFUL
#define VDSO_CGT_SYM "__kernel_clock_gettime"
#define VDSO_CGT_VER "LINUX_2.6.39"

#define IPC_64 0
