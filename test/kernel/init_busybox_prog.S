/*
 * Slice 7.6d.N.final.b — busybox-as-/init bootstrapper EL0 stub.
 *
 * Embedded into kernel-busybox.bin's .rodata between
 * __init_busybox_prog_start and __init_busybox_prog_end.  The boot path
 * (boot.c built with -DNX_INIT_BUSYBOX) creates an "init" process,
 * memcopies this blob into the process's user backing at the user-window
 * base, and drops_to_el0 at the entry.  The blob then SVCs into
 * NX_SYS_EXEC to replace its image with /init (which `make run-busybox`
 * has packed as the busybox binary via pack-initramfs.py's
 * --busybox-init flag).
 *
 * argv = { "sh", NULL }; busybox dispatches to applet name
 * basename(argv[0]) — same in-process applet trick slice 7.6d.N.0
 * established for `nxlibc_execve("/bin/busybox", { "sh", "-c", ... })`.
 *
 * Layout-identical to user_prog_exec.S (slice 7.4c): same .rodata
 * embedding, same VA-relative `adr` addressing, same "fall back to
 * exit on exec failure" pattern.  No fork/wait dance — there's no
 * parent to coordinate with.
 *
 * argv lives on the user stack because a literal `.quad _argv0_sh`
 * in .rodata would store the LINK-time absolute address of the string,
 * and the blob is copied verbatim to 0x48000000 (user-window base) at
 * runtime so that absolute would be wrong.  `adr` is PC-relative and
 * resolves to the correct runtime VA, so we adr-then-store the pointer.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #11 = NX_SYS_EXIT
 *   #14 = NX_SYS_EXEC
 */

.section .rodata
.balign 16
.global __init_busybox_prog_start
.global __init_busybox_prog_end

__init_busybox_prog_start:

    /* Build argv on the stack: argv[0] = "sh", argv[1] = NULL.
     * The init-runner sets sp_el0 to (top-of-window) & ~0xf, so
     * subtracting 16 leaves us 16-byte aligned. */
    sub     sp, sp, #16
    adr     x9, _argv0_sh
    str     x9, [sp]
    str     xzr, [sp, #8]

    /* exec("/init", argv, NULL) */
    adr     x0, _init_path
    mov     x1, sp
    mov     x2, #0                          /* envp = NULL */
    mov     x8, #14                         /* NX_SYS_EXEC */
    svc     #0

    /* Reached only on exec failure — exit so the kernel's init runner
     * stops yielding to a dead process. */
    mov     x0, #1
    mov     x8, #11                         /* NX_SYS_EXIT */
    svc     #0

_init_park:
    wfe
    b       _init_park

.balign 8
_argv0_sh:
    .asciz  "sh"

.balign 8
_init_path:
    .asciz  "/init"

__init_busybox_prog_end:
