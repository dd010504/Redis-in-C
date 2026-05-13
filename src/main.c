/*
 * main.c - entry point + Phase 1 smoke test.
 *
 * Lifecycle (mirrors the memory contract documented in hashtable.h):
 *
 *   1. g_store is a static (BSS-resident) hashtable_t. We own the
 *      storage; ht_init() only zeroes its buckets.
 *   2. ht_set() copies the key/value bytes into the table -- our local
 *      buffers can go out of scope immediately afterwards.
 *   3. ht_get() returns borrowed pointers; we read them and forget them.
 *   4. ht_destroy() frees every internal malloc before we exit so a
 *      future `valgrind --leak-check=full` will report zero in-use bytes.
 */

#include "hashtable.h"
#include "networking.h"

#include <stdio.h>
#include <string.h>

/* Single global store. BSS-allocated -- no heap involvement here, which
 * is exactly what the hashtable_t memory contract permits. */
static hashtable_t g_store;

/* Pretty-print a (possibly binary) value to stderr. We treat each byte
 * as printable-or-hex so embedded NULs don't truncate the output. */
static void dump_value(const char *label, const void *val, size_t vlen)
{
    fprintf(stderr, "  %s (%zu bytes): \"", label, vlen);
    const unsigned char *p = (const unsigned char *)val;
    for (size_t i = 0; i < vlen; ++i) {
        if (p[i] >= 0x20 && p[i] < 0x7f) {
            fputc((int)p[i], stderr);
        } else {
            fprintf(stderr, "\\x%02x", (unsigned)p[i]);
        }
    }
    fputs("\"\n", stderr);
}

/* Exercise SET/GET/DEL once and report. Binary-safe API: we always pass
 * explicit lengths so embedded NULs are preserved end-to-end. */
static void smoke_test(void)
{
    fprintf(stderr, "[smoke] running Phase 1 hashtable smoke test...\n");

    /* SET two text-ish keys. */
    if (ht_set(&g_store, "hello", 5, "world", 5) != 0) {
        fprintf(stderr, "[smoke] ht_set(hello) failed (OOM)\n");
        return;
    }
    if (ht_set(&g_store, "name", 4, "redis-in-c", 10) != 0) {
        fprintf(stderr, "[smoke] ht_set(name) failed (OOM)\n");
        return;
    }

    /* SET a binary-safe key with an embedded NUL to prove the API isn't
     * pretending C strings under the hood. `unsigned char` so byte
     * literals like 0xff don't trip -Woverflow on platforms where
     * `char` is signed (i.e. most of them). */
    const unsigned char bin_key[] = { 'a', 0x00, 'b' };          /* length 3 */
    const unsigned char bin_val[] = { 0x01, 0x02, 0x00, 0xff };  /* length 4 */
    if (ht_set(&g_store, bin_key, sizeof bin_key, bin_val, sizeof bin_val) != 0) {
        fprintf(stderr, "[smoke] ht_set(binary) failed (OOM)\n");
        return;
    }

    /* GET them back. The returned pointers are borrowed from the table. */
    const void *val = NULL;
    size_t      vlen = 0;

    if (ht_get(&g_store, "hello", 5, &val, &vlen)) {
        dump_value("hello", val, vlen);
    } else {
        fprintf(stderr, "  hello: <missing>\n");
    }

    if (ht_get(&g_store, "name", 4, &val, &vlen)) {
        dump_value("name", val, vlen);
    } else {
        fprintf(stderr, "  name: <missing>\n");
    }

    if (ht_get(&g_store, bin_key, sizeof bin_key, &val, &vlen)) {
        dump_value("a\\x00b", val, vlen);
    } else {
        fprintf(stderr, "  a\\x00b: <missing>\n");
    }

    /* Overwrite to prove the value buffer is freed and replaced. */
    if (ht_set(&g_store, "hello", 5, "WORLD!", 6) == 0
        && ht_get(&g_store, "hello", 5, &val, &vlen)) {
        dump_value("hello (overwritten)", val, vlen);
    }

    /* DEL and verify the GET miss path. */
    int deleted = ht_del(&g_store, "name", 4);
    fprintf(stderr, "  ht_del(name): %s\n", deleted ? "ok" : "miss");
    int hit = ht_get(&g_store, "name", 4, &val, &vlen);
    fprintf(stderr, "  ht_get(name) after delete: %s\n", hit ? "FOUND (bug!)" : "miss (expected)");

    fprintf(stderr, "[smoke] live entries: %zu\n", g_store.count);
}

int main(void)
{
    /* Step 1: bring the store online. No heap activity. */
    ht_init(&g_store);

    /* Step 2: prove SET/GET/DEL work before we even think about sockets. */
    smoke_test();

    /* Step 3: wire (placeholder) networking. In Phase 2 this is where
     * we'll hand the store to the epoll loop and call net_run() to
     * block on epoll_wait(). For now both are no-op stubs. */
    if (net_init(&g_store) != 0) {
        fprintf(stderr, "[main] net_init failed\n");
        ht_destroy(&g_store);
        return 1;
    }
    int rc = net_run();

    /* Step 4: tear down. ht_destroy() releases every malloc made by
     * ht_set(); the hashtable_t struct itself is in BSS and goes away
     * with the process. */
    ht_destroy(&g_store);
    return rc;
}
