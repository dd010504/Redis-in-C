/*
 * hashtable.h - binary-safe chained hash table (Phase 1: "the Brain")
 *
 * Design goals
 * ------------
 *  - Fixed-size bucket array (no rehashing in Phase 1) with separate-chaining
 *    via singly-linked lists. Keeps the implementation small and the memory
 *    behaviour easy to reason about.
 *  - Binary-safe: keys and values are arbitrary byte buffers identified by
 *    an explicit length. This matches Redis semantics and lets the Phase 2
 *    RESP parser hand slices straight to ht_set / ht_get without copying
 *    through C strings (which would choke on embedded NULs).
 *  - Single-threaded: there is NO internal locking. The event-loop owns the
 *    hashtable for the lifetime of the process.
 *
 * Memory ownership (read this before calling anything!)
 * -----------------------------------------------------
 *  - The `hashtable_t` struct itself is CALLER-OWNED. Put it on the stack,
 *    in BSS, or malloc it yourself. ht_init() only zeroes the buckets; it
 *    does not allocate the struct. ht_destroy() frees every entry it owns
 *    but it does NOT free the hashtable_t.
 *
 *  - On ht_set(): the table makes its OWN copy of the key bytes and value
 *    bytes (two malloc()s, plus one for the entry node). The caller may
 *    free or reuse its input buffers immediately after the call returns.
 *
 *  - On ht_get(): the table returns a BORROWED pointer to its internal
 *    value buffer. Do NOT free it. The pointer is valid only until the
 *    next ht_set(same key) / ht_del(same key) / ht_destroy(). If you need
 *    the value to outlive those, memcpy it.
 *
 *  - On ht_del() and ht_destroy(): all internal allocations are released.
 */

#ifndef REDIS_IN_C_HASHTABLE_H
#define REDIS_IN_C_HASHTABLE_H

#include <stddef.h>
#include <stdint.h>

/* Fixed bucket count. Power of two so `hash & (N-1)` would also work, but
 * we use `%` for clarity since this is a learning project. */
#define HT_NUM_BUCKETS 1024

/* A single key/value entry living in a bucket's chain.
 *
 * Memory layout note: `key` and `val` are separate heap allocations rather
 * than flexible array members so we can free/replace the value on overwrite
 * without disturbing the key buffer. */
typedef struct ht_entry {
    char            *key;   /* owned: malloc()'d copy of the key bytes        */
    size_t           klen;  /* length of `key` in bytes (no implicit NUL)     */
    char            *val;   /* owned: malloc()'d copy of the value bytes      */
    size_t           vlen;  /* length of `val` in bytes (no implicit NUL)     */
    struct ht_entry *next;  /* next entry in this bucket's chain, or NULL     */
} ht_entry_t;

/* The hash table itself: a fixed array of chain heads plus a counter. */
typedef struct {
    ht_entry_t *buckets[HT_NUM_BUCKETS]; /* chain heads, NULL when empty       */
    size_t      count;                   /* total live entries (for stats)     */
} hashtable_t;

/* Initialize an existing hashtable_t (zero buckets, zero count).
 * Does not allocate. Safe to call on a stack-allocated or static struct. */
void ht_init(hashtable_t *ht);

/* Free every entry owned by the table. Leaves the hashtable_t itself intact
 * (re-usable after another ht_init()), but freeing the struct is the
 * caller's job if it was heap-allocated. */
void ht_destroy(hashtable_t *ht);

/* Insert or overwrite a key.
 *
 * The table copies both the key and value buffers internally; the caller
 * retains ownership of its inputs. Passing vlen == 0 with val == NULL is
 * allowed and stores an empty value.
 *
 * Returns 0 on success, -1 on allocation failure (the table is left
 * unchanged on failure -- no partial inserts). */
int ht_set(hashtable_t *ht,
           const void *key, size_t klen,
           const void *val, size_t vlen);

/* Look up a key.
 *
 * On hit: writes a borrowed pointer into *val_out and the length into
 *         *vlen_out, and returns 1. The pointer is owned by the table;
 *         do NOT free it and do NOT keep it past the next mutation.
 * On miss: leaves *val_out / *vlen_out untouched and returns 0.
 *
 * Either output pointer may be NULL if the caller only wants existence. */
int ht_get(const hashtable_t *ht,
           const void *key, size_t klen,
           const void **val_out, size_t *vlen_out);

/* Remove a key. Returns 1 if a matching entry was deleted, 0 otherwise. */
int ht_del(hashtable_t *ht,
           const void *key, size_t klen);

/* DJB2 hash (binary-safe variant: `hash * 33 ^ byte`).
 *
 * Exposed so unit tests and main.c diagnostics can probe distribution
 * without re-implementing the algorithm. */
uint64_t ht_hash(const void *key, size_t klen);

#endif /* REDIS_IN_C_HASHTABLE_H */
