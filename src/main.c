/*
 * main.c - entry point + smoke tests for Phase 1 (hashtable) and
 *          Phase 2 step 2 (RESP request parser).
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
 *
 * The RESP parser is library code only at this step -- the smoke test
 * exercises it directly here; networking.c will start calling it in
 * Phase 2 step 3 (command dispatch).
 */

#include "hashtable.h"
#include "networking.h"
#include "resp.h"

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

/* ------------------------------------------------------------------------- */
/* Phase 2 step 2 - RESP parser smoke test                                   */
/* ------------------------------------------------------------------------- */

/* Tally so we can print a one-line summary at the end. */
static int  g_resp_total = 0;
static int  g_resp_pass  = 0;

/* Byte-exact slice comparison. `expected` is `const void *` to avoid
 * sign-conversion warnings when callers hand us either `char[]` literals
 * or `unsigned char[]` brace-initialised buffers. */
static int slice_eq(const resp_slice_t *s, const void *expected, size_t expected_len)
{
    return s->len == expected_len && memcmp(s->bytes, expected, expected_len) == 0;
}

#define RESP_CHECK(name, cond) do {                                       \
    g_resp_total++;                                                       \
    if (cond) {                                                           \
        g_resp_pass++;                                                    \
        fprintf(stderr, "  [resp] %-30s OK\n",   (name));                 \
    } else {                                                              \
        fprintf(stderr, "  [resp] %-30s FAIL\n", (name));                 \
    }                                                                     \
} while (0)

static void resp_smoke_test(void)
{
    fprintf(stderr, "[smoke] running Phase 2 step 2 RESP parser smoke test...\n");

    /* 1. Multi-bulk: SET foo bar (the redis-cli wire form). */
    {
        const unsigned char buf[] = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("mb: SET foo bar",
            st == RESP_OK
            && r.argc == 3
            && slice_eq(&r.args[0], "SET", 3)
            && slice_eq(&r.args[1], "foo", 3)
            && slice_eq(&r.args[2], "bar", 3)
            && r.bytes_consumed == sizeof buf - 1);
    }

    /* 2. Multi-bulk: PING with a single arg. */
    {
        const unsigned char buf[] = "*1\r\n$4\r\nPING\r\n";
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("mb: PING",
            st == RESP_OK && r.argc == 1 && slice_eq(&r.args[0], "PING", 4));
    }

    /* 3. Inline form (what `nc` would type). */
    {
        const unsigned char buf[] = "PING\r\n";
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("inline: PING",
            st == RESP_OK && r.argc == 1 && slice_eq(&r.args[0], "PING", 4));
    }

    /* 4. Inline with mixed whitespace + bare \n leniency. */
    {
        const unsigned char buf[] = "SET\tfoo  bar\n";  /* tab, double space, bare \n */
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("inline: tabs/spaces/\\n",
            st == RESP_OK
            && r.argc == 3
            && slice_eq(&r.args[0], "SET", 3)
            && slice_eq(&r.args[1], "foo", 3)
            && slice_eq(&r.args[2], "bar", 3));
    }

    /* 5. Multi-bulk with a binary-safe value containing NUL and 0xff. */
    {
        const unsigned char buf[] = {
            '*','3','\r','\n',
            '$','3','\r','\n','S','E','T','\r','\n',
            '$','3','\r','\n','f','o','o','\r','\n',
            '$','5','\r','\n','a', 0x00, 'b', 0xff, 'c','\r','\n'
        };
        const unsigned char expected_val[] = { 'a', 0x00, 'b', 0xff, 'c' };
        resp_request_t      r              = {0};
        resp_status_t       st             = resp_parse_request(buf, sizeof buf, &r);
        RESP_CHECK("mb: SET foo <binary>",
            st == RESP_OK
            && r.argc == 3
            && slice_eq(&r.args[0], "SET", 3)
            && slice_eq(&r.args[1], "foo", 3)
            && slice_eq(&r.args[2], expected_val, sizeof expected_val));
    }

    /* 6. Partial frame: same as case 1 but with the trailing \r\n
     *    chopped off. Should report NEED_MORE without touching *out. */
    {
        const unsigned char buf[] = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar";
        resp_request_t      r     = {0};
        const size_t        sentinel = (size_t)0xdead;
        r.argc = sentinel;   /* must remain unchanged on NEED_MORE */
        resp_status_t st = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("partial frame (NEED_MORE)",
            st == RESP_NEED_MORE && r.argc == sentinel);
    }

    /* 7. Negative array count -- nil array, illegal in a request. */
    {
        const unsigned char buf[] = "*-1\r\n";
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("*-1 nil array (PROTOCOL_ERR)", st == RESP_PROTOCOL_ERR);
    }

    /* 8. Bulk header says 2 bytes but body is 4: the post-body \r\n
     *    lands on payload bytes instead. */
    {
        const unsigned char buf[] = "*1\r\n$2\r\nABCD\r\n";
        resp_request_t      r     = {0};
        resp_status_t       st    = resp_parse_request(buf, sizeof buf - 1, &r);
        RESP_CHECK("bulk overrun (PROTOCOL_ERR)", st == RESP_PROTOCOL_ERR);
    }

    fprintf(stderr, "[resp] %d / %d cases passed\n", g_resp_pass, g_resp_total);
}

int main(void)
{
    /* Step 1: bring the store online. No heap activity. */
    ht_init(&g_store);

    /* Step 2: prove SET/GET/DEL work before we even think about sockets. */
    smoke_test();

    /* Step 3: prove the RESP parser handles the wire forms it'll see
     * from redis-cli (multi-bulk) and nc (inline) once step 3 of Phase 2
     * wires it into the event loop. */
    resp_smoke_test();

    /* Step 4: bring up the listener and enter the epoll loop. Blocks
     * until SIGINT/SIGTERM. */
    if (net_init(&g_store) != 0) {
        fprintf(stderr, "[main] net_init failed\n");
        ht_destroy(&g_store);
        return 1;
    }
    int rc = net_run();

    /* Step 5: tear down. ht_destroy() releases every malloc made by
     * ht_set(); the hashtable_t struct itself is in BSS and goes away
     * with the process. */
    ht_destroy(&g_store);
    return rc;
}
