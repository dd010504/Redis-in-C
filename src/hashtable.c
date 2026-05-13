/*
 * hashtable.c - implementation of the binary-safe chained hash table.
 *
 * Allocation summary (every malloc has a matching free pinned below):
 *
 *   site                       allocates              freed by
 *   ----------------------     --------------------   -----------------------
 *   ht_set (new insert)        ht_entry_t node        ht_del / ht_destroy
 *   ht_set (new insert)        key byte buffer        ht_del / ht_destroy
 *   ht_set (insert/overwrite)  value byte buffer      ht_set (overwrite) /
 *                                                     ht_del / ht_destroy
 *
 * There are no other heap allocations. ht_init, ht_get, ht_hash, and the
 * lookup paths are all allocation-free.
 */

#include "hashtable.h"

#include <stdlib.h>   /* malloc, free                   */
#include <string.h>   /* memcpy, memcmp                 */

/* ------------------------------------------------------------------------- */
/* Hashing                                                                   */
/* ------------------------------------------------------------------------- */

/* DJB2 with XOR mixing (Daniel J. Bernstein, comp.lang.c circa 1991).
 *
 * The classic formulation is `hash = hash * 33 + c`, but the XOR variant
 * (`hash * 33 ^ c`) tends to distribute slightly better on short binary
 * keys. We use a 64-bit accumulator so we can mask down to any bucket
 * count later without overflow surprises. */
uint64_t ht_hash(const void *key, size_t klen)
{
    const unsigned char *p = (const unsigned char *)key;
    uint64_t hash = 5381u;

    for (size_t i = 0; i < klen; ++i) {
        /* hash * 33 ^ byte, expressed as (hash << 5) + hash for speed. */
        hash = ((hash << 5) + hash) ^ (uint64_t)p[i];
    }
    return hash;
}

/* Map a 64-bit hash to a bucket index. Centralized so we can swap the
 * mapping (e.g. to `hash & (HT_NUM_BUCKETS - 1)`) in one place. */
static size_t bucket_index(uint64_t hash)
{
    return (size_t)(hash % (uint64_t)HT_NUM_BUCKETS);
}

/* Binary-safe key equality. memcmp is undefined when either pointer is NULL
 * even if the length is zero, so guard the zero-length case explicitly. */
static int keys_equal(const void *a, size_t alen, const void *b, size_t blen)
{
    if (alen != blen) {
        return 0;
    }
    if (alen == 0) {
        return 1;
    }
    return memcmp(a, b, alen) == 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void ht_init(hashtable_t *ht)
{
    /* No heap work here -- just stamp the buckets to NULL. The struct
     * itself was provided by the caller. */
    for (size_t i = 0; i < HT_NUM_BUCKETS; ++i) {
        ht->buckets[i] = NULL;
    }
    ht->count = 0;
}

/* Internal helper: release a single entry's owned memory.
 *
 * Pairs with the three malloc()s inside ht_set(). After this returns the
 * entry pointer is dangling and must not be dereferenced. */
static void free_entry(ht_entry_t *e)
{
    /* free(NULL) is well-defined, but being explicit makes the ownership
     * audit above easier to read. */
    free(e->key);   /* matches the malloc in ht_set (key buffer)   */
    free(e->val);   /* matches the malloc in ht_set (value buffer) */
    free(e);        /* matches the malloc in ht_set (node)         */
}

void ht_destroy(hashtable_t *ht)
{
    /* Walk every chain and free every entry. We do NOT touch the
     * hashtable_t itself -- the caller owns that storage. */
    for (size_t i = 0; i < HT_NUM_BUCKETS; ++i) {
        ht_entry_t *cur = ht->buckets[i];
        while (cur != NULL) {
            ht_entry_t *next = cur->next;
            free_entry(cur);
            cur = next;
        }
        ht->buckets[i] = NULL;
    }
    ht->count = 0;
}

/* ------------------------------------------------------------------------- */
/* Mutation                                                                  */
/* ------------------------------------------------------------------------- */

/* Allocate and copy a byte buffer. Returns NULL on OOM or on (NULL, 0)
 * input we still want to represent as an empty heap allocation so that
 * free() works uniformly later.
 *
 * For an empty value we malloc(1) instead of malloc(0) because malloc(0)
 * is implementation-defined; the one wasted byte is a deliberate trade
 * for predictable behaviour. */
static char *dup_bytes(const void *src, size_t len)
{
    char *buf = (char *)malloc(len == 0 ? 1u : len);
    if (buf == NULL) {
        return NULL;
    }
    if (len > 0 && src != NULL) {
        memcpy(buf, src, len);
    }
    return buf;
}

int ht_set(hashtable_t *ht,
           const void *key, size_t klen,
           const void *val, size_t vlen)
{
    const size_t idx = bucket_index(ht_hash(key, klen));

    /* First pass: look for an existing entry to overwrite in place. This
     * avoids the cost of allocating a new node when the key already
     * exists, and (more importantly) preserves the table on OOM. */
    for (ht_entry_t *cur = ht->buckets[idx]; cur != NULL; cur = cur->next) {
        if (keys_equal(cur->key, cur->klen, key, klen)) {
            /* Allocate the replacement value FIRST. If it fails we still
             * have the original value intact and can report -1 cleanly. */
            char *new_val = dup_bytes(val, vlen);
            if (new_val == NULL) {
                return -1;
            }
            /* Free the old value buffer (allocated by a previous ht_set)
             * and swap in the new one. Key buffer and node stay put. */
            free(cur->val);
            cur->val  = new_val;
            cur->vlen = vlen;
            return 0;
        }
    }

    /* No existing entry -- build a fresh node. Allocate everything up
     * front so we can unwind cleanly on partial OOM. */
    ht_entry_t *node = (ht_entry_t *)malloc(sizeof(*node));
    if (node == NULL) {
        return -1;
    }
    char *key_copy = dup_bytes(key, klen);
    if (key_copy == NULL) {
        free(node);          /* undo the node malloc above */
        return -1;
    }
    char *val_copy = dup_bytes(val, vlen);
    if (val_copy == NULL) {
        free(key_copy);      /* undo the key malloc above  */
        free(node);          /* undo the node malloc above */
        return -1;
    }

    node->key  = key_copy;
    node->klen = klen;
    node->val  = val_copy;
    node->vlen = vlen;

    /* Prepend to the chain: O(1), and recently-touched keys sit at the
     * front which gives a mild LRU-ish locality benefit for lookups. */
    node->next         = ht->buckets[idx];
    ht->buckets[idx]   = node;
    ht->count++;
    return 0;
}

int ht_del(hashtable_t *ht,
           const void *key, size_t klen)
{
    const size_t idx = bucket_index(ht_hash(key, klen));

    /* Walk the chain with a pointer-to-pointer trick so we can unlink
     * the head and any interior node with one branch-free assignment. */
    ht_entry_t **link = &ht->buckets[idx];
    while (*link != NULL) {
        ht_entry_t *cur = *link;
        if (keys_equal(cur->key, cur->klen, key, klen)) {
            *link = cur->next;   /* unlink                          */
            free_entry(cur);     /* releases node + key + val bufs  */
            ht->count--;
            return 1;
        }
        link = &cur->next;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lookup                                                                    */
/* ------------------------------------------------------------------------- */

int ht_get(const hashtable_t *ht,
           const void *key, size_t klen,
           const void **val_out, size_t *vlen_out)
{
    const size_t idx = bucket_index(ht_hash(key, klen));

    for (ht_entry_t *cur = ht->buckets[idx]; cur != NULL; cur = cur->next) {
        if (keys_equal(cur->key, cur->klen, key, klen)) {
            if (val_out != NULL) {
                *val_out = cur->val;     /* borrowed: do NOT free */
            }
            if (vlen_out != NULL) {
                *vlen_out = cur->vlen;
            }
            return 1;
        }
    }
    return 0;
}
