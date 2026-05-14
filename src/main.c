/*
 * main.c - entry point + smoke tests for Phase 1 (hashtable),
 *          Phase 2 step 2 (RESP parser), and Phase 2 step 3
 *          (command dispatcher).
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
 * The three smoke tests run at startup before the epoll loop so any
 * regression in the parser, the dispatcher, or the hashtable shows
 * up as a FAIL line before the server starts accepting clients.
 */

#include "bytebuf.h"
#include "command.h"
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

/* ------------------------------------------------------------------------- */
/* Phase 2 step 3 - command dispatcher smoke test                            */
/* ------------------------------------------------------------------------- */

static int g_cmd_total = 0;
static int g_cmd_pass  = 0;

/* Byte-exact comparison of the entire bytebuf payload against an
 * expected literal. `expected` is `const void *` for the same reason
 * slice_eq does it: callers may pass `char[]` or `unsigned char[]`. */
static int bytebuf_eq(const bytebuf_t *b, const void *expected, size_t expected_len)
{
    if (bytebuf_len(b) != expected_len) {
        return 0;
    }
    if (expected_len == 0) {
        return 1;
    }
    return memcmp(bytebuf_data(b), expected, expected_len) == 0;
}

/* Prefix check -- handy for KEYS * where the trailing element order
 * is hashtable-bucket-dependent so byte-exact comparison would be
 * brittle. */
static int bytebuf_starts_with(const bytebuf_t *b, const void *prefix, size_t plen)
{
    if (bytebuf_len(b) < plen) {
        return 0;
    }
    if (plen == 0) {
        return 1;
    }
    return memcmp(bytebuf_data(b), prefix, plen) == 0;
}

#define CMD_CHECK(name, cond) do {                                        \
    g_cmd_total++;                                                        \
    if (cond) {                                                           \
        g_cmd_pass++;                                                     \
        fprintf(stderr, "  [cmd]  %-30s OK\n",   (name));                 \
    } else {                                                              \
        fprintf(stderr, "  [cmd]  %-30s FAIL\n", (name));                 \
    }                                                                     \
} while (0)

static void command_smoke_test(void)
{
    fprintf(stderr, "[smoke] running Phase 2 step 3 dispatcher smoke test...\n");

    bytebuf_t out;
    bytebuf_init(&out);
    command_ctx_t ctx = { .store = &g_store, .out = &out };

    /* 1. PING -> +PONG\r\n */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"PING", 4 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("PING", rc == 0 && bytebuf_eq(&out, "+PONG\r\n", 7));
        bytebuf_reset(&out);
    }

    /* 2. PING <msg> -> bulk reply */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"PING",  4 },
                { (const unsigned char *)"hello", 5 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("PING hello", rc == 0 && bytebuf_eq(&out, "$5\r\nhello\r\n", 11));
        bytebuf_reset(&out);
    }

    /* 3. SET smoke_foo bar -> +OK\r\n */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"SET",        3 },
                { (const unsigned char *)"smoke_foo",  9 },
                { (const unsigned char *)"bar",        3 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("SET smoke_foo bar", rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 4. GET smoke_foo -> $3\r\nbar\r\n */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET",        3 },
                { (const unsigned char *)"smoke_foo",  9 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET smoke_foo", rc == 0 && bytebuf_eq(&out, "$3\r\nbar\r\n", 9));
        bytebuf_reset(&out);
    }

    /* 5. GET smoke_nope -> $-1\r\n (nil) */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET",         3 },
                { (const unsigned char *)"smoke_nope", 10 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET smoke_nope (miss)", rc == 0 && bytebuf_eq(&out, "$-1\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 6. EXISTS smoke_foo smoke_nope smoke_foo -> :2\r\n
     *    (duplicates are counted, per real Redis semantics) */
    {
        resp_request_t req = {
            .argc = 4,
            .args = {
                { (const unsigned char *)"EXISTS",      6 },
                { (const unsigned char *)"smoke_foo",   9 },
                { (const unsigned char *)"smoke_nope", 10 },
                { (const unsigned char *)"smoke_foo",   9 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("EXISTS foo nope foo", rc == 0 && bytebuf_eq(&out, ":2\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 7. DEL smoke_foo -> :1\r\n */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"DEL",        3 },
                { (const unsigned char *)"smoke_foo",  9 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DEL smoke_foo", rc == 0 && bytebuf_eq(&out, ":1\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 8. GET smoke_foo after DEL -> $-1\r\n */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET",        3 },
                { (const unsigned char *)"smoke_foo",  9 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET smoke_foo after DEL", rc == 0 && bytebuf_eq(&out, "$-1\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 9. COMMAND -> *0\r\n (empty array; placates newer redis-cli) */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"COMMAND", 7 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("COMMAND -> empty array", rc == 0 && bytebuf_eq(&out, "*0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 10. Unknown command -> -ERR unknown command 'XYZ'\r\n */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"XYZ", 3 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("XYZ (unknown command)",
            rc == 0 && bytebuf_eq(&out, "-ERR unknown command 'XYZ'\r\n", 28));
        bytebuf_reset(&out);
    }

    /* 11. SET with one arg -> wrong number of arguments error */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"SET",      3 },
                { (const unsigned char *)"only_key", 8 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("SET wrong arity",
            rc == 0 && bytebuf_eq(&out,
                                  "-ERR wrong number of arguments for 'SET'\r\n",
                                  42));
        bytebuf_reset(&out);
    }

    /* --------------------------------------------------------------------- */
    /* Phase 3: integer arithmetic                                           */
    /* --------------------------------------------------------------------- */

    /* 12. FLUSHDB wipes the slate left by Phase 1's smoke test (which
     *     populated "hello" and the binary "a\\x00b" key) and lets us
     *     reason about subsequent cases from a clean state. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"FLUSHDB", 7 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("FLUSHDB (reset state)", rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 13. INCR on a fresh key starts from 0 -> :1 */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"INCR",    4 },
                { (const unsigned char *)"counter", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("INCR counter (new)", rc == 0 && bytebuf_eq(&out, ":1\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 14. INCR again bumps the existing value to 2. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"INCR",    4 },
                { (const unsigned char *)"counter", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("INCR counter", rc == 0 && bytebuf_eq(&out, ":2\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 15. INCRBY counter 10 -> :12 */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"INCRBY",  6 },
                { (const unsigned char *)"counter", 7 },
                { (const unsigned char *)"10",      2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("INCRBY counter 10", rc == 0 && bytebuf_eq(&out, ":12\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 16. DECRBY counter 5 -> :7 */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"DECRBY",  6 },
                { (const unsigned char *)"counter", 7 },
                { (const unsigned char *)"5",       1 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DECRBY counter 5", rc == 0 && bytebuf_eq(&out, ":7\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 17. DECR counter -> :6 */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"DECR",    4 },
                { (const unsigned char *)"counter", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DECR counter", rc == 0 && bytebuf_eq(&out, ":6\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 18. Plant a non-integer value so INCR can refuse it next. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"SET",     3 },
                { (const unsigned char *)"not_int", 7 },
                { (const unsigned char *)"hello",   5 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("SET not_int hello", rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 19. INCR on a non-integer value yields the standard Redis error. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"INCR",    4 },
                { (const unsigned char *)"not_int", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("INCR not_int (error)",
            rc == 0 && bytebuf_eq(&out,
                "-ERR value is not an integer or out of range\r\n", 46));
        bytebuf_reset(&out);
    }

    /* --------------------------------------------------------------------- */
    /* Phase 3: string utility                                               */
    /* --------------------------------------------------------------------- */

    /* 20. counter currently holds "6" (1 byte). */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"STRLEN",  6 },
                { (const unsigned char *)"counter", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("STRLEN counter", rc == 0 && bytebuf_eq(&out, ":1\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 21. STRLEN on a missing key returns 0 (per real Redis). */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"STRLEN",  6 },
                { (const unsigned char *)"missing", 7 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("STRLEN missing", rc == 0 && bytebuf_eq(&out, ":0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 22. APPEND on a new key behaves like SET, returns the length. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"APPEND",   6 },
                { (const unsigned char *)"greeting", 8 },
                { (const unsigned char *)"Hello",    5 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("APPEND greeting Hello (new)",
            rc == 0 && bytebuf_eq(&out, ":5\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 23. APPEND to an existing key concatenates and returns new length. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"APPEND",   6 },
                { (const unsigned char *)"greeting", 8 },
                { (const unsigned char *)" World",   6 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("APPEND greeting \" World\"",
            rc == 0 && bytebuf_eq(&out, ":11\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 24. GET greeting -> "Hello World" (proves the concat worked). */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET",      3 },
                { (const unsigned char *)"greeting", 8 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET greeting (after APPEND)",
            rc == 0 && bytebuf_eq(&out, "$11\r\nHello World\r\n", 18));
        bytebuf_reset(&out);
    }

    /* --------------------------------------------------------------------- */
    /* Phase 3: keyspace (KEYS / DBSIZE / FLUSHDB)                            */
    /* --------------------------------------------------------------------- */

    /* 25. Clean slate before the keyspace cases. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"FLUSHDB", 7 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("FLUSHDB (pre-keyspace)",
            rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 26. DBSIZE on an empty keyspace. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"DBSIZE", 6 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DBSIZE (empty)", rc == 0 && bytebuf_eq(&out, ":0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 27-29. Seed three known keys. */
    {
        const char *keys[3] = { "k1", "k2", "k3" };
        const char *vals[3] = { "v1", "v2", "v3" };
        for (int i = 0; i < 3; ++i) {
            resp_request_t req = {
                .argc = 3,
                .args = {
                    { (const unsigned char *)"SET", 3 },
                    { (const unsigned char *)keys[i], 2 },
                    { (const unsigned char *)vals[i], 2 }
                }
            };
            int rc = command_execute(&ctx, &req);
            char label[32];
            snprintf(label, sizeof label, "SET %s %s", keys[i], vals[i]);
            CMD_CHECK(label, rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
            bytebuf_reset(&out);
        }
    }

    /* 30. DBSIZE now reports 3. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"DBSIZE", 6 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DBSIZE (3 keys)", rc == 0 && bytebuf_eq(&out, ":3\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 31. KEYS k1 (literal, no glob metacharacters) matches exactly one. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"KEYS", 4 },
                { (const unsigned char *)"k1",   2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("KEYS k1 (literal match)",
            rc == 0 && bytebuf_eq(&out, "*1\r\n$2\r\nk1\r\n", 12));
        bytebuf_reset(&out);
    }

    /* 32. KEYS nope* matches nothing. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"KEYS",  4 },
                { (const unsigned char *)"nope*", 5 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("KEYS nope* (no match)",
            rc == 0 && bytebuf_eq(&out, "*0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 33. KEYS * returns all 3 keys. Hashtable bucket order is
     *     implementation-defined, so we check the array header and
     *     total length rather than byte-equal a fixed ordering. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"KEYS", 4 },
                { (const unsigned char *)"*",    1 }
            }
        };
        int rc = command_execute(&ctx, &req);
        /* "*3\r\n" header (4 bytes) + 3 * "$2\r\nXX\r\n" (8 bytes each) = 28. */
        CMD_CHECK("KEYS * (3 keys, any order)",
            rc == 0
            && bytebuf_starts_with(&out, "*3\r\n", 4)
            && bytebuf_len(&out) == 28);
        bytebuf_reset(&out);
    }

    /* 34. FLUSHDB returns the world to empty. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"FLUSHDB", 7 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("FLUSHDB (post-keyspace)",
            rc == 0 && bytebuf_eq(&out, "+OK\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 35. ...and DBSIZE confirms it. */
    {
        resp_request_t req = {
            .argc = 1,
            .args = { { (const unsigned char *)"DBSIZE", 6 } }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("DBSIZE (post-flush)",
            rc == 0 && bytebuf_eq(&out, ":0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* --------------------------------------------------------------------- */
    /* Phase 3: atomic variants                                              */
    /* --------------------------------------------------------------------- */

    /* 36. SETNX on a missing key sets and returns :1. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"SETNX", 5 },
                { (const unsigned char *)"sk",    2 },
                { (const unsigned char *)"first", 5 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("SETNX sk first (new)", rc == 0 && bytebuf_eq(&out, ":1\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 37. SETNX on an existing key does nothing, returns :0. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"SETNX",  5 },
                { (const unsigned char *)"sk",     2 },
                { (const unsigned char *)"second", 6 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("SETNX sk second (no-op)",
            rc == 0 && bytebuf_eq(&out, ":0\r\n", 4));
        bytebuf_reset(&out);
    }

    /* 38. The original value is still in place. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET", 3 },
                { (const unsigned char *)"sk",  2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET sk (after SETNX no-op)",
            rc == 0 && bytebuf_eq(&out, "$5\r\nfirst\r\n", 11));
        bytebuf_reset(&out);
    }

    /* 39. GETSET on a missing key returns nil and creates it. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"GETSET", 6 },
                { (const unsigned char *)"gs",     2 },
                { (const unsigned char *)"v1",     2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GETSET gs v1 (new)",
            rc == 0 && bytebuf_eq(&out, "$-1\r\n", 5));
        bytebuf_reset(&out);
    }

    /* 40. GETSET on an existing key returns the old value and overwrites. */
    {
        resp_request_t req = {
            .argc = 3,
            .args = {
                { (const unsigned char *)"GETSET", 6 },
                { (const unsigned char *)"gs",     2 },
                { (const unsigned char *)"v2",     2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GETSET gs v2 (replace)",
            rc == 0 && bytebuf_eq(&out, "$2\r\nv1\r\n", 8));
        bytebuf_reset(&out);
    }

    /* 41. Confirm the new value landed. */
    {
        resp_request_t req = {
            .argc = 2,
            .args = {
                { (const unsigned char *)"GET", 3 },
                { (const unsigned char *)"gs",  2 }
            }
        };
        int rc = command_execute(&ctx, &req);
        CMD_CHECK("GET gs (after GETSET)",
            rc == 0 && bytebuf_eq(&out, "$2\r\nv2\r\n", 8));
        bytebuf_reset(&out);
    }

    bytebuf_free(&out);

    fprintf(stderr, "[cmd]  %d / %d cases passed\n", g_cmd_pass, g_cmd_total);
}

int main(void)
{
    /* Step 1: bring the store online. No heap activity. */
    ht_init(&g_store);

    /* Step 2: prove SET/GET/DEL work before we even think about sockets. */
    smoke_test();

    /* Step 3: prove the RESP parser handles the wire forms it'll see
     * from redis-cli (multi-bulk) and nc (inline). */
    resp_smoke_test();

    /* Step 4: prove the command dispatcher produces the exact reply
     * bytes redis-cli expects (and the right errors on miss/arity). */
    command_smoke_test();

    /* Step 5: bring up the listener and enter the epoll loop. Blocks
     * until SIGINT/SIGTERM. */
    if (net_init(&g_store) != 0) {
        fprintf(stderr, "[main] net_init failed\n");
        ht_destroy(&g_store);
        return 1;
    }
    int rc = net_run();

    /* Step 6: tear down. ht_destroy() releases every malloc made by
     * ht_set(); the hashtable_t struct itself is in BSS and goes away
     * with the process. */
    ht_destroy(&g_store);
    return rc;
}
