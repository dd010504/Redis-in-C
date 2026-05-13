/*
 * bytebuf.h - growable byte buffer with FIFO consumption.
 *
 * Used in two roles by the event loop:
 *
 *   - Read side: bytes arriving from the socket accumulate at the
 *     tail (`bytebuf_reserve` + `bytebuf_advance`). The RESP parser
 *     reads from the head via `bytebuf_data` / `bytebuf_len`, and
 *     `bytebuf_consume` advances the head pointer once a full frame
 *     has been dispatched.
 *
 *   - Write side: RESP reply serializers append into the tail. The
 *     write() loop reads from the head and consumes drained bytes.
 *
 * Layout
 * ------
 *   data: ............ off ............ len ............ cap
 *         |<- already   |<- live data  |<- spare tail   |
 *         |   consumed                                  |
 *
 * Memory contract
 * ---------------
 *   - The `bytebuf_t` struct itself is caller-owned (typically lives
 *     inside another struct, e.g. conn_t). `bytebuf_init` does NOT
 *     allocate; it just zeroes the fields.
 *   - `bytebuf_append`, `bytebuf_appendf`, and `bytebuf_reserve` may
 *     `realloc()` the underlying byte array. On allocation failure
 *     (OOM or `BYTEBUF_MAX` exceeded) they return -1 / NULL without
 *     modifying the buffer.
 *   - `bytebuf_free` releases the byte array and zeroes the struct.
 *     Safe to call on a freshly-init'd buffer (free(NULL) is a no-op).
 *   - `bytebuf_consume` never allocates; it just advances the head
 *     and lazily compacts when the wasted head space crosses a
 *     threshold.
 */

#ifndef REDIS_IN_C_BYTEBUF_H
#define REDIS_IN_C_BYTEBUF_H

#include <stddef.h>

/* Initial capacity on first growth -- small to avoid waste on
 * connections that send only a few bytes before disconnecting. */
#define BYTEBUF_INIT_CAP     256u

/* Hard cap on any single buffer. 16 MiB is way more than any sane
 * Redis command/reply needs and stops a buggy or hostile client
 * from forcing us to keep growing. */
#define BYTEBUF_MAX          (16u * 1024u * 1024u)

/* Compaction policy: when more than this many head bytes have been
 * consumed AND the head waste exceeds half the capacity, we memmove
 * live data down to offset 0. The lower bound stops us from doing
 * lots of tiny memmoves for small payloads. */
#define BYTEBUF_COMPACT_MIN  4096u

typedef struct {
    unsigned char *data;
    size_t         cap;   /* allocated capacity   */
    size_t         len;   /* total bytes written  */
    size_t         off;   /* bytes consumed from head */
} bytebuf_t;

/* Lifecycle ---------------------------------------------------------- */

void bytebuf_init(bytebuf_t *b);
void bytebuf_free(bytebuf_t *b);
void bytebuf_reset(bytebuf_t *b);   /* logically clear; keep cap */

/* Producer side (writers append at the tail) ------------------------ */

/* Copy `n` bytes from `src` to the tail. Returns 0 on success, -1 on
 * OOM or BYTEBUF_MAX exceeded. n == 0 is a no-op success. */
int bytebuf_append(bytebuf_t *b, const void *src, size_t n);

/* printf-style append. Returns 0 on success, -1 on OOM, formatting
 * error, or BYTEBUF_MAX exceeded. */
int bytebuf_appendf(bytebuf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Lower-level pair for callers that want to write straight into the
 * tail (e.g. read() from a socket). Reserve at least `n` bytes of
 * tail space and return a pointer to it; NULL on OOM/cap. The buffer
 * length does NOT advance -- after writing `k` bytes (k <= n), the
 * caller must call bytebuf_advance(b, k). */
unsigned char *bytebuf_reserve(bytebuf_t *b, size_t n);
void           bytebuf_advance(bytebuf_t *b, size_t n);

/* Consumer side (readers walk the head) ----------------------------- */

/* Pointer to the first live byte. Returns NULL only when both cap
 * and len are zero (no allocation has happened yet). */
const unsigned char *bytebuf_data(const bytebuf_t *b);

/* Number of live (unconsumed) bytes. */
size_t bytebuf_len(const bytebuf_t *b);

/* Advance the head pointer by `n` bytes (clamped). May compact the
 * buffer in-place. Never allocates, never fails. */
void bytebuf_consume(bytebuf_t *b, size_t n);

#endif /* REDIS_IN_C_BYTEBUF_H */
