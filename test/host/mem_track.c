#include "mem_track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Block layout:
 *
 *   [ mt_block  ][ redzone_pre ][ payload ][ redzone_post ]
 *                                ^
 *                                user pointer
 *
 * States:
 *   LIVE      — block is allocated; listed in `live_head`
 *   FREED     — caller called mt_free; block is in `quarantine_head`; payload
 *               filled with MT_UAF_BYTE. Double-free and write-after-free are
 *               detected against this state.
 */

#define MT_MAGIC 0x4E58544BU  /* "NXTK" — sanity check on user ptrs */

enum mt_state { MT_LIVE = 1, MT_FREED = 2 };

struct mt_block {
    struct mt_block *next;
    struct mt_block *prev;
    size_t           size;
    const char      *file;
    int              line;
    enum mt_state    state;
    unsigned         magic;
    /* then: redzone_pre, payload, redzone_post */
};

static struct mt_block *live_head;
static struct mt_block *quarantine_head;
static size_t           live_count;

static unsigned char *payload_of(struct mt_block *b)
{
    return (unsigned char *)(b + 1) + MT_REDZONE_SIZE;
}

static struct mt_block *block_of(void *user_ptr)
{
    unsigned char *p = (unsigned char *)user_ptr;
    return (struct mt_block *)(p - MT_REDZONE_SIZE - sizeof(struct mt_block));
}

static int redzones_intact(struct mt_block *b)
{
    unsigned char *pre  = (unsigned char *)(b + 1);
    unsigned char *post = payload_of(b) + b->size;
    for (size_t i = 0; i < MT_REDZONE_SIZE; i++) {
        if (pre[i]  != MT_REDZONE_BYTE) return 0;
        if (post[i] != MT_REDZONE_BYTE) return 0;
    }
    return 1;
}

static int uaf_bytes_intact(struct mt_block *b)
{
    unsigned char *p = payload_of(b);
    for (size_t i = 0; i < b->size; i++)
        if (p[i] != MT_UAF_BYTE) return 0;
    return 1;
}

void *mt_alloc(size_t size, const char *file, int line)
{
    size_t total = sizeof(struct mt_block) + MT_REDZONE_SIZE + size + MT_REDZONE_SIZE;
    struct mt_block *b = malloc(total);
    if (!b) return NULL;

    b->size  = size;
    b->file  = file;
    b->line  = line;
    b->state = MT_LIVE;
    b->magic = MT_MAGIC;

    memset((unsigned char *)(b + 1),       MT_REDZONE_BYTE, MT_REDZONE_SIZE);
    memset(payload_of(b) + size,           MT_REDZONE_BYTE, MT_REDZONE_SIZE);
    memset(payload_of(b),                  0,               size);

    b->prev = NULL;
    b->next = live_head;
    if (live_head) live_head->prev = b;
    live_head = b;
    live_count++;

    return payload_of(b);
}

void mt_free(void *ptr, const char *file, int line)
{
    if (!ptr) return;

    struct mt_block *b = block_of(ptr);
    if (b->magic != MT_MAGIC) {
        fprintf(stderr, "mem_track: free of non-tracked pointer %p at %s:%d\n",
                ptr, file, line);
        abort();
    }

    if (b->state == MT_FREED) {
        fprintf(stderr,
                "mem_track: DOUBLE-FREE of %p at %s:%d (previously freed at %s:%d)\n",
                ptr, file, line, b->file, b->line);
        abort();
    }

    if (!redzones_intact(b)) {
        fprintf(stderr,
                "mem_track: OVERFLOW: red zone corrupted on block %p "
                "(size=%zu, allocated at %s:%d, freed at %s:%d)\n",
                ptr, b->size, b->file, b->line, file, line);
        abort();
    }

    /* unlink from live list */
    if (b->prev) b->prev->next = b->next;
    else         live_head     = b->next;
    if (b->next) b->next->prev = b->prev;
    live_count--;

    /* quarantine */
    memset(payload_of(b), MT_UAF_BYTE, b->size);
    b->state = MT_FREED;
    b->file  = file;
    b->line  = line;
    b->prev  = NULL;
    b->next  = quarantine_head;
    if (quarantine_head) quarantine_head->prev = b;
    quarantine_head = b;
}

void mt_reset(void)
{
    /* Release everything back to libc; the next test starts clean. */
    for (struct mt_block *b = live_head; b; ) {
        struct mt_block *next = b->next;
        free(b);
        b = next;
    }
    for (struct mt_block *b = quarantine_head; b; ) {
        struct mt_block *next = b->next;
        free(b);
        b = next;
    }
    live_head       = NULL;
    quarantine_head = NULL;
    live_count      = 0;
}

int mt_check_leaks(void)
{
    int problems = 0;

    for (struct mt_block *b = live_head; b; b = b->next) {
        problems++;
        fprintf(stderr,
                "mem_track: LEAK: %zu bytes allocated at %s:%d, never freed\n",
                b->size, b->file, b->line);
        if (!redzones_intact(b)) {
            fprintf(stderr,
                    "              ... red zones also corrupted\n");
        }
    }

    for (struct mt_block *b = quarantine_head; b; b = b->next) {
        if (!uaf_bytes_intact(b)) {
            problems++;
            fprintf(stderr,
                    "mem_track: WRITE-AFTER-FREE into block freed at %s:%d "
                    "(%zu bytes, originally allocated elsewhere)\n",
                    b->file, b->line, b->size);
        }
        if (!redzones_intact(b)) {
            problems++;
            fprintf(stderr,
                    "mem_track: OVERFLOW: red zone corrupted on freed block "
                    "freed at %s:%d\n",
                    b->file, b->line);
        }
    }

    return problems;
}

size_t mt_live_count(void)
{
    return live_count;
}
