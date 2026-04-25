/*
 * Slice 7.6c.3c — musl-linked printf demo.
 *
 * Validates that musl's stdio surface (printf + the implicit malloc
 * for stdio buffers + locale state) runs end-to-end on nonux.  This
 * is the first demo to actually pull mallocng + locale init into
 * the link — those paths exercise mmap/brk/clock_gettime that
 * earlier demos avoided.
 *
 * The exit code (67) is distinct from posix_libc_prog (53),
 * posix_musl_prog (57), posix_main_prog (47), posix_printf_prog
 * (37) so a regression in any one is immediately pinpointable in
 * the ktest summary.
 *
 * Inline forward declarations to avoid pulling in <stdio.h> /
 * <stdlib.h> headers — those drag the rest of musl's header tree
 * which is more than this demo needs.  The signatures match
 * musl's exactly.
 */

extern int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void exit(int status) __attribute__((noreturn));

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* Single printf with multiple conversions — covers %d, %s, %x,
     * %u, %c.  musl's vfprintf walks the format string, builds the
     * output in a stdio buffer, and write(1)'s it on flush.
     *
     * Use exit() (not _exit()) so the atexit-driven stdio flush
     * runs before NX_SYS_EXIT.  Without it the line-buffered
     * output sits in stdout's buffer and gets discarded — the
     * program's exit_code propagates fine but nothing reaches
     * UART, defeating the test's marker assertion.  exit() is
     * what real C programs use for clean shutdown anyway.
     */
    printf("[musl-printf d=%d u=%u x=%x s=%s c=%c]\n",
           -1, 4294967295u, 0xdeadbeef, "ok", 'Q');

    exit(67);
}
