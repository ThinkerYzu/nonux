/*
 * Host tests for framework/elf.{h,c} (slice 7.3).
 *
 * The parser is pure data code and runs on both host and kernel;
 * these tests construct synthetic ELF blobs byte-by-byte and
 * verify the parser's behaviour without needing an MMU.  The
 * loader half (`nx_elf_load_into_process`) is kernel-only and has
 * a host stub that only re-validates + reflects the entry point —
 * that stub is exercised here too.
 */

#include "test_runner.h"

#include "framework/elf.h"
#include "framework/process.h"
#include "framework/registry.h"

#include <stdint.h>
#include <string.h>

/* ---------- Synthetic ELF builder ------------------------------------ */
/*
 * Build an ELF64 AArch64 ET_EXEC blob with exactly `nloads` PT_LOAD
 * program headers + an extra non-LOAD pad header so the parser's
 * filter logic sees something to skip.  Layout:
 *
 *     [0..64)   ELF header
 *     [64..64+56*(nloads+1))  program header table (one pad + nloads LOADs)
 *     segment bytes packed immediately after
 *
 * Returned buffer is statically allocated up to 4 KiB — plenty for
 * the <= 4 segment cases these tests exercise.
 */

static uint8_t g_buf[4096];

struct elf64_hdr_fields {
    uint64_t entry;
    uint64_t phoff;
    uint16_t phnum;
};

static size_t build_blob(unsigned nloads, uint64_t entry,
                         uint64_t first_vaddr,
                         uint32_t seg_filesz, uint32_t seg_memsz)
{
    memset(g_buf, 0, sizeof g_buf);

    /* ELF header — match every field elf.c validates. */
    g_buf[0] = 0x7F; g_buf[1] = 'E'; g_buf[2] = 'L'; g_buf[3] = 'F';
    g_buf[4] = 2;    /* ELFCLASS64 */
    g_buf[5] = 1;    /* ELFDATA2LSB */

    /* e_type = ET_EXEC (2) at offset 16. */
    g_buf[16] = 2;    g_buf[17] = 0;
    /* e_machine = EM_AARCH64 (183) at offset 18. */
    g_buf[18] = 183;  g_buf[19] = 0;
    /* e_version = 1 at offset 20. */
    g_buf[20] = 1;

    /* e_entry at offset 24 (8 bytes LE). */
    for (int i = 0; i < 8; i++)
        g_buf[24 + i] = (uint8_t)((entry >> (i * 8)) & 0xff);

    /* e_phoff at offset 32 — right after the 64-byte header. */
    uint64_t phoff = 64;
    for (int i = 0; i < 8; i++)
        g_buf[32 + i] = (uint8_t)((phoff >> (i * 8)) & 0xff);

    /* e_ehsize (52) = 64, e_phentsize (54) = 56, e_phnum (56) =
     * (nloads + 1).  The "+1" is a non-LOAD pad slot to exercise
     * the filter. */
    g_buf[52] = 64; g_buf[53] = 0;
    g_buf[54] = 56; g_buf[55] = 0;
    uint16_t phnum = (uint16_t)(nloads + 1);
    g_buf[56] = (uint8_t)(phnum & 0xff);
    g_buf[57] = (uint8_t)((phnum >> 8) & 0xff);

    /* Pad phdr first: p_type = 0 (PT_NULL), everything else zero. */
    /* (already zero from memset) */

    /* Each of the nloads LOAD phdrs at offset 64 + 56*(i+1). */
    uint64_t seg_offset = 64 + (uint64_t)56 * phnum;
    for (unsigned i = 0; i < nloads; i++) {
        uint8_t *p = g_buf + 64 + 56 * (i + 1);
        /* p_type = 1 (PT_LOAD). */
        p[0] = 1;
        /* p_flags = 5 (RE). */
        p[4] = 5;
        /* p_offset at offset 8 (8 bytes LE). */
        for (int j = 0; j < 8; j++)
            p[8 + j] = (uint8_t)((seg_offset >> (j * 8)) & 0xff);
        /* p_vaddr at offset 16 (8 bytes LE). */
        uint64_t vaddr = first_vaddr + (uint64_t)i * 0x1000;
        for (int j = 0; j < 8; j++)
            p[16 + j] = (uint8_t)((vaddr >> (j * 8)) & 0xff);
        /* p_paddr at offset 24 — same as vaddr is harmless. */
        for (int j = 0; j < 8; j++)
            p[24 + j] = (uint8_t)((vaddr >> (j * 8)) & 0xff);
        /* p_filesz at offset 32. */
        for (int j = 0; j < 4; j++)
            p[32 + j] = (uint8_t)((seg_filesz >> (j * 8)) & 0xff);
        /* p_memsz at offset 40. */
        for (int j = 0; j < 4; j++)
            p[40 + j] = (uint8_t)((seg_memsz >> (j * 8)) & 0xff);
        /* p_align at offset 48 — 4 KiB. */
        p[48] = 0; p[49] = 0x10;

        seg_offset += seg_filesz;
    }
    return (size_t)seg_offset;
}

/* ---------- Tests --------------------------------------------------- */

TEST(elf_parse_accepts_valid_single_load)
{
    size_t n = build_blob(1, 0x48000000, 0x48000000, 16, 32);
    struct nx_elf_info info = {0};
    int rc = nx_elf_parse(g_buf, n, &info);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(info.entry, 0x48000000);
    ASSERT_EQ_U(info.segment_count, 1);
}

TEST(elf_parse_skips_non_load_phdrs_in_count)
{
    /* build_blob always prepends one non-LOAD phdr; nloads=3 means
     * the parser should see 3 PT_LOADs and NOT count the pad. */
    size_t n = build_blob(3, 0x48000000, 0x48000000, 8, 8);
    struct nx_elf_info info = {0};
    ASSERT_EQ_U(nx_elf_parse(g_buf, n, &info), NX_OK);
    ASSERT_EQ_U(info.segment_count, 3);
}

TEST(elf_segment_iterates_by_dense_index)
{
    size_t n = build_blob(3, 0x48000000, 0x48000000, 8, 8);

    for (uint16_t i = 0; i < 3; i++) {
        struct nx_elf_segment seg = {0};
        int rc = nx_elf_segment(g_buf, n, i, &seg);
        ASSERT_EQ_U(rc, NX_OK);
        /* Each segment we built has vaddr = base + i * 0x1000. */
        ASSERT_EQ_U(seg.vaddr, 0x48000000 + (uint64_t)i * 0x1000);
        ASSERT_EQ_U(seg.file_size, 8);
        ASSERT_EQ_U(seg.mem_size,  8);
    }

    /* Out-of-range index returns NX_EINVAL. */
    struct nx_elf_segment tail;
    ASSERT_EQ_U((uint64_t)nx_elf_segment(g_buf, n, 99, &tail),
                (uint64_t)(int64_t)NX_EINVAL);
}

TEST(elf_parse_rejects_bad_magic)
{
    size_t n = build_blob(1, 0x48000000, 0x48000000, 8, 8);
    g_buf[0] = 0xFE;   /* not 0x7F */
    struct nx_elf_info info;
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, n, &info),
                (uint64_t)(int64_t)NX_EINVAL);
}

TEST(elf_parse_rejects_wrong_class_or_encoding)
{
    size_t n = build_blob(1, 0x48000000, 0x48000000, 8, 8);
    uint8_t save_class = g_buf[4];
    g_buf[4] = 1;      /* ELFCLASS32 — unsupported */
    struct nx_elf_info info;
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, n, &info),
                (uint64_t)(int64_t)NX_EINVAL);
    g_buf[4] = save_class;

    g_buf[5] = 2;      /* ELFDATA2MSB — unsupported */
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, n, &info),
                (uint64_t)(int64_t)NX_EINVAL);
}

TEST(elf_parse_rejects_wrong_type_or_machine)
{
    size_t n = build_blob(1, 0x48000000, 0x48000000, 8, 8);
    /* Flip e_type from ET_EXEC (2) to ET_DYN (3). */
    g_buf[16] = 3;
    struct nx_elf_info info;
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, n, &info),
                (uint64_t)(int64_t)NX_EINVAL);
    g_buf[16] = 2;

    /* Flip e_machine from EM_AARCH64 (183) to EM_X86_64 (62). */
    g_buf[18] = 62;
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, n, &info),
                (uint64_t)(int64_t)NX_EINVAL);
}

TEST(elf_parse_rejects_truncated_blob)
{
    size_t n = build_blob(1, 0x48000000, 0x48000000, 8, 8);
    struct nx_elf_info info;
    /* Truncated to half the header — must reject. */
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, 32, &info),
                (uint64_t)(int64_t)NX_EINVAL);
    /* Truncated past header but phoff+phnum*phentsize extends past
     * the truncation — must reject. */
    ASSERT_EQ_U((uint64_t)nx_elf_parse(g_buf, 80, &info),
                (uint64_t)(int64_t)NX_EINVAL);
    /* Full blob accepted. */
    ASSERT_EQ_U(nx_elf_parse(g_buf, n, &info), NX_OK);
}

TEST(elf_load_into_process_host_stub_reflects_entry)
{
    /* Host loader is a pure-validation stub (no MMU).  It should
     * still return the entry for callers that want to verify the
     * blob is well-formed. */
    size_t n = build_blob(1, 0x48000000, 0x48000000, 16, 16);

    struct nx_process *p = nx_process_create("elf-host");
    ASSERT_NOT_NULL(p);

    uint64_t entry = 0;
    int rc = nx_elf_load_into_process(p, g_buf, n, &entry);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(entry, 0x48000000);

    nx_process_destroy(p);
}
