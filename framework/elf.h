#ifndef NX_FRAMEWORK_ELF_H
#define NX_FRAMEWORK_ELF_H

#include "framework/process.h"
#include "framework/registry.h"

#include <stddef.h>
#include <stdint.h>

/*
 * ELF64 parser + loader — slice 7.3.
 *
 * First consumer of `struct nx_process`'s private address space
 * (slice 7.2): this module takes an in-memory ELF blob, validates
 * it as a static AArch64 executable, and copies every PT_LOAD
 * segment into the target process's user-window backing.  The
 * caller gets back the ELF's entry point and can `drop_to_el0`
 * into it.
 *
 * What we support.
 *   - 64-bit ELF (EI_CLASS == ELFCLASS64).
 *   - Little-endian (EI_DATA == ELFDATA2LSB).
 *   - ET_EXEC (static executable) with EM_AARCH64.
 *   - Any number of PT_LOAD segments, as long as every one lands
 *     entirely inside the process's user window
 *     (`mmu_user_window_base() .. +mmu_user_window_size()`).
 *
 * What we DON'T support in v1.
 *   - ET_DYN (position-independent / shared objects).  Slice 7.3
 *     is for static executables linked at a known VA.
 *   - Relocations.  Static executables have their absolute
 *     addresses baked in; we just copy bytes.
 *   - Symbol tables, debug info.  Loader ignores every section
 *     that isn't PT_LOAD.
 *   - BSS zeroing beyond the user window.  If `p_memsz` extends
 *     past the window end, load returns NX_ENOMEM.
 *
 * Kernel-vs-host.
 *   The parser (header/phdr validation, iteration) is pure-data
 *   code and runs on both builds — host tests construct synthetic
 *   blobs and verify the parser returns the expected shape.  The
 *   loader calls into `core/mmu/` to reach the process's user
 *   backing, so the loader as such only runs on kernel.  Host
 *   builds stub the loader entry point; `nx_elf_parse` is fully
 *   exercised on host.
 */

/* ---------- Error codes ---------------------------------------------- */
/*
 * Uses the same `NX_E*` namespace as the rest of the framework
 * (see framework/registry.h).  Parser- / loader-specific errors
 * are mapped:
 *   - `NX_EINVAL` — bad ELF magic, unsupported class/encoding/
 *     machine, or a malformed program header.
 *   - `NX_ENOMEM` — a PT_LOAD segment wouldn't fit in the
 *     target's user window.
 *   - `NX_OK` — parse / load succeeded.
 */

/* ---------- Parser API ------------------------------------------------ */

/*
 * Shape-data extracted from an ELF64 header.  Consumers that just
 * need to look at the entry point and segment count use this;
 * consumers that need to iterate segments call `nx_elf_segment`.
 */
struct nx_elf_info {
    uint64_t entry;        /* program entry point VA */
    uint16_t segment_count; /* number of PT_LOAD segments */
};

/*
 * One PT_LOAD segment's shape as reported by the parser.  All
 * fields come straight from the program header with minimal
 * massaging.
 */
struct nx_elf_segment {
    uint64_t file_offset;  /* where segment bytes start in the blob */
    uint64_t vaddr;        /* target VA in the process's address space */
    uint64_t file_size;    /* bytes to copy from blob */
    uint64_t mem_size;     /* total bytes the segment occupies;
                            * (mem_size - file_size) is zero-fill */
    uint32_t flags;        /* raw p_flags (PF_R | PF_W | PF_X) */
};

/*
 * Validate `blob` as an ELF64-AArch64 ET_EXEC image and read its
 * header shape into `*out`.  Returns NX_OK on success, NX_EINVAL
 * on any validation failure.
 *
 * `out` may be NULL — callers that only want to validate can
 * ignore the shape.  `len` must be at least `sizeof(elf_header)`;
 * shorter blobs return NX_EINVAL.
 */
int nx_elf_parse(const void *blob, size_t len, struct nx_elf_info *out);

/*
 * Read the `idx`-th PT_LOAD segment's shape from a validated
 * blob.  `idx` runs from 0 to `info.segment_count - 1`
 * (inclusive-exclusive; non-PT_LOAD program headers are
 * skipped during iteration so the caller's index matches what
 * `segment_count` reported).
 *
 * Returns NX_OK on success (with `*out` populated), NX_EINVAL
 * on bad blob or out-of-range index.
 *
 * The caller is responsible for validating that
 * `file_offset + file_size <= len` before dereferencing — the
 * parser notes the blob shape but doesn't walk the bytes.
 */
int nx_elf_segment(const void *blob, size_t len, uint16_t idx,
                   struct nx_elf_segment *out);

/* ---------- Loader API ----------------------------------------------- */

/*
 * Load every PT_LOAD segment from `blob` into `target`'s address
 * space.  On success writes the ELF entry VA to `*out_entry` and
 * returns NX_OK.
 *
 * Semantics:
 *   - Segment bytes go to `target`'s user-window backing (the
 *     contiguous 2 MiB blocks the MMU layer carved out for this
 *     process — currently 4 blocks = 8 MiB).  The VA stays within
 *     `[mmu_user_window_base(), +size)`; anything that crosses
 *     either boundary returns NX_ENOMEM.
 *   - `mem_size - file_size` bytes past the copied region are
 *     zeroed (BSS).
 *   - The loader does NOT flip TTBR0 during the copy.  It writes
 *     via the kernel-visible identity-map alias returned by
 *     `mmu_address_space_user_backing(target->ttbr0_root)`, so
 *     the target's pages get populated without the kernel
 *     leaving its own TTBR0 view.
 *
 * Errors:
 *   NX_EINVAL — NULL args, malformed ELF, or a segment outside
 *               the user window.
 *   NX_ENOMEM — as above + BSS-fill would overflow the window.
 */
int nx_elf_load_into_process(struct nx_process *target,
                             const void *blob, size_t len,
                             uint64_t *out_entry);

#endif /* NX_FRAMEWORK_ELF_H */
