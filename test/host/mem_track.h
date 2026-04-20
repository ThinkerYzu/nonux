#ifndef NONUX_MEM_TRACK_H
#define NONUX_MEM_TRACK_H

#include <stddef.h>

/*
 * mem_track — host-only memory-tracking allocator for component tests.
 *
 * Wraps malloc/free with:
 *   - allocation site (file:line) and size recorded per block
 *   - red zones before/after each block (REDZONE_SIZE bytes of REDZONE_BYTE)
 *   - double-free detection (freed blocks are quarantined, not returned to libc
 *     until mt_reset())
 *   - simple use-after-free detection (freed payload is filled with UAF_BYTE;
 *     red-zone and content checks at next mt_check_leaks catch rewrites)
 *   - leak reporting on mt_check_leaks
 *
 * Tests use these through the TRACKED_ALLOC / TRACKED_FREE macros so that
 * __FILE__ and __LINE__ point at the call site.  kmalloc-style components
 * will #define their allocator to TRACKED_ALLOC in the test build.
 */

#define MT_REDZONE_SIZE 16
#define MT_REDZONE_BYTE 0xFE
#define MT_UAF_BYTE     0xDE

void *mt_alloc(size_t size, const char *file, int line);
void  mt_free(void *ptr,   const char *file, int line);

/* Clear tracking state and release quarantined blocks to libc. Call at the
 * start of every test. */
void mt_reset(void);

/* Print diagnostics for any block that is still allocated or whose red zones
 * were corrupted. Returns the count of problems found. */
int  mt_check_leaks(void);

/* Number of live (non-quarantined) allocations. Used for lifecycle tests that
 * check "enable acquires, disable releases." */
size_t mt_live_count(void);

#define TRACKED_ALLOC(sz) mt_alloc((sz), __FILE__, __LINE__)
#define TRACKED_FREE(p)   mt_free((p),  __FILE__, __LINE__)

#endif /* NONUX_MEM_TRACK_H */
