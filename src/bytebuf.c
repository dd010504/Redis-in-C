/*
 * bytebuf.c - growable byte buffer implementation.
 *
 * Allocation summary (every malloc has a matching free pinned below):
 *
 *   site                                   allocates       freed by
 *   ------------------------------------   --------------- ---------------
 *   ensure_tail_cap (first growth)         data array      bytebuf_free
 *   ensure_tail_cap (subsequent growth)    new data array  bytebuf_free
 *                                                          (old array is
 *                                                          freed by realloc
 *                                                          itself)
 *
 * Nothing else here heap-allocates; bytebuf_consume / bytebuf_reset /
 * bytebuf_init / bytebuf_advance / accessors are all O(1) (consume
 * may memmove on compaction but does not alloc).
 */

#include "bytebuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static void compact_in_place(bytebuf_t *b)
{
    /* Move live bytes (data + off .. data + len) down to offset 0. */
    size_t live = b->len - b->off;
    if (live > 0 && b->off > 0) {
        memmove(b->data, b->data + b->off, live);
    }
    b->len = live;
    b->off = 0;
}

/* Make sure there is room for at least `need` more bytes after the
 * tail. May compact the head (free), reallocate (grow), or both.
 *
 * Returns 0 on success, -1 on OOM or BYTEBUF_MAX exceeded. */
static int ensure_tail_cap(bytebuf_t *b, size_t need)
{
    /* Cheap path: enough tail already. */
    if (b->cap - b->len >= need) {
        return 0;
    }

    /* Try to reclaim head space first before paying for a realloc. */
    if (b->off > 0) {
        compact_in_place(b);
        if (b->cap - b->len >= need) {
            return 0;
        }
    }

    /* Need a bigger backing array. Double until we fit (or hit cap). */
    size_t new_cap = (b->cap == 0) ? BYTEBUF_INIT_CAP : b->cap;
    while (new_cap - b->len < need) {
        if (new_cap >= BYTEBUF_MAX) {
            return -1;   /* would exceed the hard cap */
        }
        if (new_cap > BYTEBUF_MAX / 2u) {
            new_cap = BYTEBUF_MAX;   /* one final jump to the cap */
        } else {
            new_cap *= 2u;
        }
    }

    unsigned char *new_data = (unsigned char *)realloc(b->data, new_cap);
    if (new_data == NULL) {
        return -1;
    }
    b->data = new_data;
    b->cap  = new_cap;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void bytebuf_init(bytebuf_t *b)
{
    b->data = NULL;
    b->cap  = 0;
    b->len  = 0;
    b->off  = 0;
}

void bytebuf_free(bytebuf_t *b)
{
    free(b->data);   /* matches the (re)alloc in ensure_tail_cap */
    b->data = NULL;
    b->cap  = 0;
    b->len  = 0;
    b->off  = 0;
}

void bytebuf_reset(bytebuf_t *b)
{
    b->len = 0;
    b->off = 0;
    /* `data` and `cap` deliberately preserved -- callers reset to
     * reuse the existing allocation. */
}

/* ------------------------------------------------------------------------- */
/* Tail (producer) side                                                      */
/* ------------------------------------------------------------------------- */

int bytebuf_append(bytebuf_t *b, const void *src, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (ensure_tail_cap(b, n) < 0) {
        return -1;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int bytebuf_appendf(bytebuf_t *b, const char *fmt, ...)
{
    /* First attempt into a small stack buffer to avoid double-format
     * in the common case (short replies like "+OK\r\n", ":42\r\n"). */
    char    stack[128];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return -1;   /* encoding error */
    }
    if ((size_t)n < sizeof stack) {
        return bytebuf_append(b, stack, (size_t)n);
    }

    /* Didn't fit -- reserve exact size (plus 1 for vsnprintf's NUL)
     * and format directly into the bytebuf. */
    if (ensure_tail_cap(b, (size_t)n + 1u) < 0) {
        return -1;
    }
    va_start(ap, fmt);
    int n2 = vsnprintf((char *)(b->data + b->len), (size_t)n + 1u, fmt, ap);
    va_end(ap);
    if (n2 < 0 || n2 != n) {
        return -1;
    }
    b->len += (size_t)n;   /* skip the trailing NUL */
    return 0;
}

unsigned char *bytebuf_reserve(bytebuf_t *b, size_t n)
{
    if (ensure_tail_cap(b, n) < 0) {
        return NULL;
    }
    return b->data + b->len;
}

void bytebuf_advance(bytebuf_t *b, size_t n)
{
    /* Caller's responsibility to ensure they wrote within the
     * reserved region -- we just bump len. Clamp defensively. */
    if (n > b->cap - b->len) {
        n = b->cap - b->len;
    }
    b->len += n;
}

/* ------------------------------------------------------------------------- */
/* Head (consumer) side                                                      */
/* ------------------------------------------------------------------------- */

const unsigned char *bytebuf_data(const bytebuf_t *b)
{
    /* When data is NULL (no allocation yet), returning NULL is fine
     * because bytebuf_len() also returns 0 -- callers shouldn't deref
     * either way. */
    return b->data == NULL ? NULL : b->data + b->off;
}

size_t bytebuf_len(const bytebuf_t *b)
{
    return b->len - b->off;
}

void bytebuf_consume(bytebuf_t *b, size_t n)
{
    size_t live = b->len - b->off;
    if (n > live) {
        n = live;
    }
    b->off += n;

    /* Fully drained: reset the offsets (no memmove needed). */
    if (b->off == b->len) {
        b->off = 0;
        b->len = 0;
        return;
    }

    /* Otherwise lazily compact when wasted head space is significant. */
    if (b->off > BYTEBUF_COMPACT_MIN && b->off > b->cap / 2u) {
        compact_in_place(b);
    }
}
