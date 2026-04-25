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
 *   - sys_exec (slice 7.6c.4) pushes the System V argv layout onto
 *     the new user stack: argc at [sp+0], argv[0..argc-1] at
 *     [sp+8..sp+8+8*argc-1], NULL terminator, envp NULL terminator,
 *     argv strings.  crt0 reads argc from [sp]; if argc > 0, uses
 *     the stack-pushed layout.  drop_to_el0 (no-exec entry) leaves
 *     the user backing zero-initialised so [sp] reads as 0 — crt0
 *     falls back to the synthesised default `argv = { "nonux", NULL }`
 *     in `.rodata`.  envp is currently always the empty environment
 *     (envp = NULL) regardless of entry path.
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

    /* Slice 7.6c.4: read argc from [sp].  sys_exec pushes the
     * System V argv layout onto the user stack and sets sp_el0 to
     * the argc slot, so when we arrive here:
     *   [sp+ 0]                argc
     *   [sp+ 8 .. sp+8*argc]   argv[0..argc-1]
     *   [sp+8*(argc+1)]        NULL (argv terminator)
     *   [sp+8*(argc+2)]        NULL (envp[0])
     *
     * For a drop_to_el0 entry (no exec), the backing is zero-
     * initialised so [sp] reads as 0 — that's the cue to fall back
     * to the synthesised single-element argv. */
    ldr     x0, [sp]
    cbz     x0, _synth_argv

    /* Stack-pushed argv path: argv = sp+8, envp = sp + 16 + 8*argc. */
    add     x1, sp, #8
    add     x2, sp, #16
    add     x2, x2, x0, lsl #3
    b       _call_main

_synth_argv:
    /* Synthesised argv = { "nonux", NULL }, envp = NULL.  Same
     * shape as before slice 7.6c.4 — keeps the existing
     * non-exec'd EL0 demos working without a kernel-side
     * change. */
    mov     x0, #1
    adrp    x1, _argv_storage
    add     x1, x1, :lo12:_argv_storage
    mov     x2, #0

_call_main:
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
