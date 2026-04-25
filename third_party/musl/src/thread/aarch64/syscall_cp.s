// nonux patch — slice 7.6c.3a.
//
// Stock musl's __syscall_cp_asm puts x1 (the Linux syscall nr) into
// x8 and emits `svc 0` directly, bypassing the C-level
// `__syscallN -> svc` path that we patched in
// arch/aarch64/syscall_arch.h.  This file does the same translation
// inline so the cancellation-point syscall path also lands on a
// nonux NX_SYS_* number.
//
// Mapping table matches __nx_translate() in
// arch/aarch64/syscall_arch.h verbatim.  Unmapped numbers return
// -ENOSYS (-38) without entering the kernel.
//
// __cp_begin / __cp_end / __cp_cancel symbols are still emitted —
// pthread_cancel.c uses them as a PC range to detect "the syscall
// itself was interrupted by a cancel signal".  They retain their
// stock semantic (begin = function entry, end = post-svc).
// Cancellation isn't wired up in nonux's v1 (no pthread), so the
// asynchronous-cancel path stays cold; but the static linker still
// resolves the symbol references.

// __syscall_cp_asm(&self->cancel, nr, u, v, w, x, y, z)
//                  x0             x1  x2 x3 x4 x5 x6 x7

.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type __syscall_cp_asm,%function
__syscall_cp_asm:
__cp_begin:
	ldr w0,[x0]
	cbnz w0,__cp_cancel

	// Linux syscall nr in x1 -> NX_SYS_* in x8.  Unmapped -> -ENOSYS.
	cmp x1, #57;   b.eq .Lnx_close
	cmp x1, #62;   b.eq .Lnx_lseek
	cmp x1, #63;   b.eq .Lnx_read
	cmp x1, #64;   b.eq .Lnx_write
	cmp x1, #93;   b.eq .Lnx_exit
	cmp x1, #94;   b.eq .Lnx_exit
	cmp x1, #129;  b.eq .Lnx_kill
	cmp x1, #221;  b.eq .Lnx_execve
	cmp x1, #260;  b.eq .Lnx_wait
	movn x0, #37          // -ENOSYS = -38
	ret

.Lnx_close:    mov x8, #2;  b .Lnx_run    // NX_SYS_HANDLE_CLOSE
.Lnx_lseek:    mov x8, #9;  b .Lnx_run    // NX_SYS_SEEK
.Lnx_read:     mov x8, #7;  b .Lnx_run    // NX_SYS_READ
.Lnx_write:    mov x8, #8;  b .Lnx_run    // NX_SYS_WRITE
.Lnx_exit:     mov x8, #11; b .Lnx_run    // NX_SYS_EXIT
.Lnx_kill:     mov x8, #16; b .Lnx_run    // NX_SYS_SIGNAL
.Lnx_execve:   mov x8, #14; b .Lnx_run    // NX_SYS_EXEC
.Lnx_wait:     mov x8, #13;               // NX_SYS_WAIT, fall through

.Lnx_run:
	mov x0,x2
	mov x1,x3
	mov x2,x4
	mov x3,x5
	mov x4,x6
	mov x5,x7
	svc 0
__cp_end:
	ret
__cp_cancel:
	b __cancel
