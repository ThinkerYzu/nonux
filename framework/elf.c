/*
 * ELF64 parser + loader implementation — slice 7.3.  See elf.h
 * for the contract.
 *
 * The parser and the loader are kept in one file even though the
 * host build only compiles the parser half.  Guarded with
 * __STDC_HOSTED__ so the loader's MMU dependency doesn't drag
 * kernel headers into the host build.
 */

#include "framework/elf.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#include "core/mmu/mmu.h"
#endif

/* ---------- ELF64 wire-format structs --------------------------------- */
/*
 * Only the fields we consume.  Field ordering + sizes match the
 * ELF64 spec (LSB-first, naturally aligned).  The structs are
 * packed implicitly by the standard layout; no `__attribute__
 * ((packed))` is needed because ELF64 headers are already all-
 * natural-alignment for 64-bit types.
 */

#define EI_NIDENT 16

/* Byte-0..3 of e_ident: 0x7F, 'E', 'L', 'F' */
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* e_ident[EI_CLASS] values */
#define ELFCLASS64   2

/* e_ident[EI_DATA] values */
#define ELFDATA2LSB  1

/* e_type values */
#define ET_EXEC      2

/* e_machine values */
#define EM_AARCH64   183

/* p_type values */
#define PT_LOAD      1

struct elf64_hdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* ---------- Parser ---------------------------------------------------- */

static int validate_header(const struct elf64_hdr *h, size_t len)
{
    if (len < sizeof *h) return NX_EINVAL;
    if (h->e_ident[0] != ELFMAG0 || h->e_ident[1] != ELFMAG1 ||
        h->e_ident[2] != ELFMAG2 || h->e_ident[3] != ELFMAG3)
        return NX_EINVAL;
    if (h->e_ident[4] != ELFCLASS64)  return NX_EINVAL;
    if (h->e_ident[5] != ELFDATA2LSB) return NX_EINVAL;
    if (h->e_type    != ET_EXEC)      return NX_EINVAL;
    if (h->e_machine != EM_AARCH64)   return NX_EINVAL;
    if (h->e_phentsize != sizeof(struct elf64_phdr)) return NX_EINVAL;

    /* Program-header table must fit within the blob. */
    uint64_t phoff = h->e_phoff;
    uint64_t phsz  = (uint64_t)h->e_phentsize * h->e_phnum;
    if (phoff > (uint64_t)len)           return NX_EINVAL;
    if (phsz  > (uint64_t)len - phoff)   return NX_EINVAL;
    return NX_OK;
}

/* Walk the program headers and count PT_LOAD entries.  Used both
 * by `nx_elf_parse` (for the summary shape) and internally by
 * `nx_elf_segment` to turn a dense index into a phdr offset. */
static uint16_t count_pt_load(const struct elf64_hdr *h, const void *blob)
{
    uint16_t n = 0;
    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)blob + h->e_phoff);
    for (uint16_t i = 0; i < h->e_phnum; i++)
        if (phdrs[i].p_type == PT_LOAD) n++;
    return n;
}

int nx_elf_parse(const void *blob, size_t len, struct nx_elf_info *out)
{
    if (!blob) return NX_EINVAL;
    const struct elf64_hdr *h = blob;
    int rc = validate_header(h, len);
    if (rc != NX_OK) return rc;

    if (out) {
        out->entry         = h->e_entry;
        out->segment_count = count_pt_load(h, blob);
    }
    return NX_OK;
}

int nx_elf_segment(const void *blob, size_t len, uint16_t idx,
                   struct nx_elf_segment *out)
{
    if (!blob || !out) return NX_EINVAL;
    const struct elf64_hdr *h = blob;
    int rc = validate_header(h, len);
    if (rc != NX_OK) return rc;

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)((const uint8_t *)blob + h->e_phoff);

    uint16_t seen = 0;
    for (uint16_t i = 0; i < h->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (seen == idx) {
            out->file_offset = phdrs[i].p_offset;
            out->vaddr       = phdrs[i].p_vaddr;
            out->file_size   = phdrs[i].p_filesz;
            out->mem_size    = phdrs[i].p_memsz;
            out->flags       = phdrs[i].p_flags;
            return NX_OK;
        }
        seen++;
    }
    return NX_EINVAL;  /* idx out of range */
}

/* ---------- Loader ---------------------------------------------------- */

#if !__STDC_HOSTED__
/*
 * Kernel-only helper: resolve a VA in the target's user window to
 * the corresponding kernel-accessible pointer.  The MMU layer
 * hands out the 2 MiB backing chunk for a given TTBR0 root; we
 * offset into it by `(vaddr - user_window_base)`.  Bounds-checked
 * against the window size.
 *
 * Returns NULL if `vaddr` or the whole `[vaddr, vaddr+span)`
 * range falls outside the window.
 */
static void *resolve_user_va(struct nx_process *p,
                             uint64_t vaddr, uint64_t span)
{
    if (!p || !p->ttbr0_root) return NULL;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    if (vaddr < base)                       return NULL;
    if (vaddr - base > size)                return NULL;
    if (span > size - (vaddr - base))       return NULL;
    void *backing = mmu_address_space_user_backing(p->ttbr0_root);
    if (!backing) return NULL;
    return (void *)((uint8_t *)backing + (vaddr - base));
}
#endif

int nx_elf_load_into_process(struct nx_process *target,
                             const void *blob, size_t len,
                             uint64_t *out_entry)
{
#if __STDC_HOSTED__
    /* No MMU on host — the loader is purely a kernel notion.
     * Parser still validates so host tests can call this and
     * observe the entry-point reflection, but we refuse to claim
     * any bytes were copied. */
    (void)target;
    struct nx_elf_info info;
    int rc = nx_elf_parse(blob, len, &info);
    if (rc != NX_OK) return rc;
    if (out_entry) *out_entry = info.entry;
    return NX_OK;
#else
    if (!target || !blob) return NX_EINVAL;

    struct nx_elf_info info;
    int rc = nx_elf_parse(blob, len, &info);
    if (rc != NX_OK) return rc;

    for (uint16_t i = 0; i < info.segment_count; i++) {
        struct nx_elf_segment seg;
        rc = nx_elf_segment(blob, len, i, &seg);
        if (rc != NX_OK) return rc;

        /* File-offset + file_size must be within the blob. */
        if (seg.file_offset > (uint64_t)len) return NX_EINVAL;
        if (seg.file_size  > (uint64_t)len - seg.file_offset) return NX_EINVAL;
        /* mem_size must be >= file_size (BSS is non-negative). */
        if (seg.mem_size < seg.file_size) return NX_EINVAL;

        void *dst = resolve_user_va(target, seg.vaddr, seg.mem_size);
        if (!dst) return NX_ENOMEM;

        if (seg.file_size > 0)
            memcpy(dst, (const uint8_t *)blob + seg.file_offset,
                   (size_t)seg.file_size);
        if (seg.mem_size > seg.file_size)
            memset((uint8_t *)dst + seg.file_size, 0,
                   (size_t)(seg.mem_size - seg.file_size));
    }

    /* Make the writes visible before the target task starts
     * fetching instructions from them.  Data cache clean to
     * PoU, then I-cache invalidate so the new code page is
     * fetched fresh.  The mapping itself hasn't changed, so no
     * TLB flush is needed. */
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");

    if (out_entry) *out_entry = info.entry;
    return NX_OK;
#endif
}
