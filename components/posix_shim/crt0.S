/*
 * posix_shim crt0 — slice 7.6c.0.
 *
 * Bridge between `drop_to_el0`'s "PC at the ELF entry, SP at the top
 * of the user window" handoff and the C-program convention `int
 * main(int argc, char **argv, char **envp)` followed by `exit(rv)`.
 * EL0 C programs that want POSIX `main` semantics link this object
 * first so it owns the ELF entry symbol `_start`.
 *
 *   ENTRY(_start) — `_start` is at 0x48000000 (the user-window base)
 *   if linked first via `init_prog.ld`, which is exactly where
 *   `mmu_user_window_base()` returns and `drop_to_el0` lands.
 *
 * v1 contract:
 *   - sys_exec doesn't push argv/envp onto the user stack today
 *     (that's a Phase 7.6d follow-up when busybox actually parses
 *     them).  We synthesise a single-element argv `{ "nonux", NULL }`
 *     in BSS and pass argc=1; envp is the empty environment NULL
 *     pointer.  Programs that don't care just `(void)argc;
 *     (void)argv;` in their main.
 *   - Stack is whatever `drop_to_el0` set sp_el0 to (top of the user
 *     window, 16-byte-aligned).  AAPCS requires 16-byte alignment at
 *     a public-interface boundary; we maintain it with the explicit
 *     `bic sp, sp, #15` before the call (defensive — the kernel
 *     already aligns, but a future host-launched test might not).
 *   - main's return value goes to nx_posix_exit unchanged; an
 *     unreached fall-through `b _start` keeps the assembler from
 *     elide the trailing instructions.
 *
 * No environment yet (envp = NULL).  When sys_exec gains argv/envp
 * stack push, this file shrinks to just "load argc/argv from sp,
 * call main, exit(rv)".
 */

.section .rodata
.balign 8
_argv_progname:
    .asciz "nonux"
.balign 8
_argv_storage:
    .quad _argv_progname
    .quad 0                  /* argv[1] = NULL terminator */

.section .text
.balign 8
.global _start
_start:
    /* Defensive 16-byte align — drop_to_el0 already aligned, but a
     * future loader might leave fractional bytes.  ARM64 has no
     * direct `bic sp, sp, #imm` form; round-trip via x9 (a
     * caller-saved temp that AAPCS lets us clobber freely at
     * program entry, before any state is established). */
    mov     x9, sp
    and     x9, x9, #~15
    mov     sp, x9

    /* argc = 1, argv = &_argv_storage, envp = NULL.  AAPCS says the
     * first three integer arguments live in x0, x1, x2.  Loading the
     * argv address with `adrp` + `add :lo12:` is the standard
     * position-independent dance; the addend resolves at link time
     * because `init_prog.ld` pins everything at 0x48000000. */
    mov     x0, #1
    adrp    x1, _argv_storage
    add     x1, x1, :lo12:_argv_storage
    mov     x2, #0

    /* `bl main` — main is provided by the user object.  The link
     * fails clean if missing: "undefined reference to `main'". */
    bl      main

    /* main returned — pass rv (in x0) to nx_posix_exit.  The wrapper
     * is `static inline` in posix.h, but that header is C; pull in
     * the syscall directly with `svc #0` to avoid a C↔asm
     * indirection.  NX_SYS_EXIT = 11. */
    mov     x8, #11
    svc     #0

    /* Unreached.  If sys_exit ever fell through, loop so a stuck
     * program doesn't run off into garbage. */
1:  wfe
    b       1b
