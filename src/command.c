/*
 * command.c - dispatch table + per-command handlers.
 *
 * This module is glue: it translates a parsed `resp_request_t` (from
 * resp.c) into hashtable operations (from hashtable.c) and a RESP
 * reply (also formatted via resp.c) appended to a bytebuf provided
 * by the caller.
 *
 * Most handlers do not allocate -- they borrow from the request,
 * live inside the hashtable, or push into the caller's bytebuf. Two
 * commands DO need a per-call scratch allocation:
 *
 *   site                       allocates              freed by
 *   ------------------------   --------------------   --------------------
 *   cmd_append (existing key)  temp concat buffer     same handler before
 *                                                     it returns
 *   cmd_getset (existing key)  old-value snapshot     same handler before
 *                                                     it returns
 *
 * Both lifetimes are strictly contained in the handler stack frame --
 * no allocation escapes into the caller.
 *
 * Adding a new command means: write a `cmd_xxx` static handler, then
 * add one row to the `commands[]` table at the bottom. The dispatcher
 * picks it up automatically.
 */

#include "command.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Case-insensitive byte compare of a slice against a NUL-terminated
 * uppercase ASCII string. Used to match command names without having
 * to copy the slice out of the request buffer first. Non-ASCII bytes
 * compare verbatim, which is the right behaviour for Redis commands
 * (they're all ASCII). */
static int slice_eq_ci(const resp_slice_t *s, const char *upper)
{
    size_t i = 0;
    while (upper[i] != '\0') {
        if (i >= s->len) {
            return 0;
        }
        unsigned char c = s->bytes[i];
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        if (c != (unsigned char)upper[i]) {
            return 0;
        }
        i++;
    }
    return i == s->len;   /* exact length match required */
}

/* Parse `bytes[0 .. len)` as a strict signed-decimal int64.
 *
 *   - Optional leading '-'; no leading '+'.
 *   - At least one digit required.
 *   - No whitespace, no trailing junk.
 *   - Rejects 64-bit overflow.
 *
 * Returns 0 on success (writing *out), -1 on any malformed input.
 *
 * Note on INT64_MIN: this accepts the range [-(2^63 - 1), 2^63 - 1].
 * The single value INT64_MIN itself is unrepresentable through "negate
 * after parsing as positive", and we don't bother with the extra
 * special-case -- real Redis behaviour around that exact edge is
 * implementation-defined anyway and INCR will never need it. */
static int parse_int64(const void *bytes, size_t len, int64_t *out)
{
    if (len == 0) {
        return -1;
    }
    const unsigned char *p        = (const unsigned char *)bytes;
    int                  negative = 0;
    size_t               i        = 0;

    if (p[0] == '-') {
        negative = 1;
        i        = 1;
        if (len == 1) {
            return -1;   /* "-" alone */
        }
    }

    int64_t v = 0;
    for (; i < len; ++i) {
        unsigned char c = p[i];
        if (c < '0' || c > '9') {
            return -1;
        }
        /* v*10 + 9 must still fit in INT64_MAX. */
        if (v > (INT64_MAX - 9) / 10) {
            return -1;
        }
        v = v * 10 + (int64_t)(c - '0');
    }
    *out = negative ? -v : v;
    return 0;
}

/* Redis-style glob matcher. Patterns recognise:
 *
 *   *              -- match any (possibly empty) byte run, greedy
 *   ?              -- match exactly one byte
 *   [abc]          -- match any one of the listed bytes
 *   [^abc] / [!abc]-- negated class
 *   \x             -- literal escape (both inside and outside classes)
 *   any other byte -- literal match
 *
 * Recursive on '*' (typical patterns are short; we don't fight the
 * pathological "**a**a**a*b" worst case). Returns 1 on match, 0
 * otherwise. */
static int glob_match(const char *pat, size_t plen,
                      const unsigned char *str, size_t slen)
{
    while (plen > 0) {
        char pc = pat[0];

        if (pc == '*') {
            /* Collapse runs of '*' to a single one. */
            while (plen > 1 && pat[1] == '*') {
                ++pat;
                --plen;
            }
            if (plen == 1) {
                return 1;   /* trailing '*' eats the rest */
            }
            /* Try matching the rest of the pattern against every
             * suffix of the input. */
            for (size_t k = 0; k <= slen; ++k) {
                if (glob_match(pat + 1, plen - 1, str + k, slen - k)) {
                    return 1;
                }
            }
            return 0;
        }

        if (pc == '?') {
            if (slen == 0) {
                return 0;
            }
            ++str;  --slen;
            ++pat;  --plen;
            continue;
        }

        if (pc == '[') {
            ++pat;  --plen;
            int negate = 0;
            if (plen > 0 && (pat[0] == '^' || pat[0] == '!')) {
                negate = 1;
                ++pat; --plen;
            }
            int matched = 0;
            while (plen > 0 && pat[0] != ']') {
                /* '\\x' escape inside a class. */
                if (pat[0] == '\\' && plen > 1) {
                    ++pat; --plen;
                }
                if (slen > 0 && (unsigned char)pat[0] == str[0]) {
                    matched = 1;
                }
                ++pat; --plen;
            }
            if (plen == 0) {
                return 0;   /* unterminated '[' -- treat as no match */
            }
            /* Skip past the ']'. */
            ++pat; --plen;
            if (negate) {
                matched = !matched;
            }
            if (!matched || slen == 0) {
                return 0;
            }
            ++str; --slen;
            continue;
        }

        if (pc == '\\' && plen > 1) {
            ++pat; --plen;
            pc = pat[0];
        }

        if (slen == 0 || (unsigned char)pc != str[0]) {
            return 0;
        }
        ++str; --slen;
        ++pat; --plen;
    }
    return slen == 0;
}

/* ------------------------------------------------------------------------- */
/* Per-command handlers                                                      */
/* ------------------------------------------------------------------------- */

/* PING            -> +PONG\r\n
 * PING <message>  -> bulk-string echo of <message> */
static int cmd_ping(command_ctx_t *ctx, const resp_request_t *req)
{
    if (req->argc == 1) {
        return resp_reply_simple_string(ctx->out, "PONG");
    }
    return resp_reply_bulk(ctx->out, req->args[1].bytes, req->args[1].len);
}

/* GET <key> -> bulk reply or nil. */
static int cmd_get(command_ctx_t *ctx, const resp_request_t *req)
{
    const void *val  = NULL;
    size_t      vlen = 0;
    if (ht_get(ctx->store, req->args[1].bytes, req->args[1].len, &val, &vlen)) {
        return resp_reply_bulk(ctx->out, val, vlen);
    }
    return resp_reply_nil(ctx->out);
}

/* SET <key> <value> -> +OK\r\n  (no options in step 3) */
static int cmd_set(command_ctx_t *ctx, const resp_request_t *req)
{
    if (ht_set(ctx->store,
               req->args[1].bytes, req->args[1].len,
               req->args[2].bytes, req->args[2].len) != 0) {
        return resp_reply_error(ctx->out, "ERR out of memory");
    }
    return resp_reply_simple_string(ctx->out, "OK");
}

/* DEL <key> [<key> ...] -> :N\r\n  (N = keys actually removed) */
static int cmd_del(command_ctx_t *ctx, const resp_request_t *req)
{
    int64_t deleted = 0;
    for (size_t i = 1; i < req->argc; ++i) {
        if (ht_del(ctx->store, req->args[i].bytes, req->args[i].len)) {
            deleted++;
        }
    }
    return resp_reply_integer(ctx->out, deleted);
}

/* EXISTS <key> [<key> ...] -> :N\r\n  (duplicates counted, like real Redis) */
static int cmd_exists(command_ctx_t *ctx, const resp_request_t *req)
{
    int64_t found = 0;
    for (size_t i = 1; i < req->argc; ++i) {
        if (ht_get(ctx->store, req->args[i].bytes, req->args[i].len, NULL, NULL)) {
            found++;
        }
    }
    return resp_reply_integer(ctx->out, found);
}

/* COMMAND [...] -> *0\r\n
 *
 * Newer redis-cli sends `COMMAND DOCS` on startup to negotiate
 * features. We don't have any commands worth advertising yet, so we
 * reply with an empty array -- enough to make redis-cli proceed
 * without erroring out. The args are intentionally ignored. */
static int cmd_command(command_ctx_t *ctx, const resp_request_t *req)
{
    (void)req;
    return resp_reply_array_header(ctx->out, 0);
}

/* ------------------------------------------------------------------------- */
/* Phase 3: integer arithmetic                                               */
/* ------------------------------------------------------------------------- */

/* Shared INCR/DECR/INCRBY/DECRBY core. Reads the current value (0 on
 * miss), parses it as int64, applies `delta` with overflow checking,
 * stores the new value back as ASCII bytes, and replies `:N`.
 *
 * Real Redis error wording is mirrored so redis-cli's display reads
 * naturally. */
static int incrby_op(command_ctx_t *ctx, const resp_slice_t *key, int64_t delta)
{
    int64_t     cur    = 0;
    const void *cur_p  = NULL;
    size_t      curlen = 0;

    if (ht_get(ctx->store, key->bytes, key->len, &cur_p, &curlen)) {
        if (parse_int64(cur_p, curlen, &cur) != 0) {
            return resp_reply_error(ctx->out,
                "ERR value is not an integer or out of range");
        }
    }

    /* Signed-int overflow check: cur + delta must stay in int64 range. */
    if ((delta > 0 && cur > INT64_MAX - delta) ||
        (delta < 0 && cur < INT64_MIN - delta)) {
        return resp_reply_error(ctx->out,
            "ERR increment or decrement would overflow");
    }
    int64_t next = cur + delta;

    /* Format the new value back to ASCII. INT64 fits in 20 chars + sign + NUL. */
    char fmt[24];
    int  n = snprintf(fmt, sizeof fmt, "%" PRId64, next);
    if (n < 0 || n >= (int)sizeof fmt) {
        return resp_reply_error(ctx->out, "ERR internal format error");
    }

    if (ht_set(ctx->store, key->bytes, key->len, fmt, (size_t)n) != 0) {
        return resp_reply_error(ctx->out, "ERR out of memory");
    }
    return resp_reply_integer(ctx->out, next);
}

static int cmd_incr(command_ctx_t *ctx, const resp_request_t *req)
{
    return incrby_op(ctx, &req->args[1], 1);
}

static int cmd_decr(command_ctx_t *ctx, const resp_request_t *req)
{
    return incrby_op(ctx, &req->args[1], -1);
}

static int cmd_incrby(command_ctx_t *ctx, const resp_request_t *req)
{
    int64_t delta;
    if (parse_int64(req->args[2].bytes, req->args[2].len, &delta) != 0) {
        return resp_reply_error(ctx->out,
            "ERR value is not an integer or out of range");
    }
    return incrby_op(ctx, &req->args[1], delta);
}

static int cmd_decrby(command_ctx_t *ctx, const resp_request_t *req)
{
    int64_t delta;
    if (parse_int64(req->args[2].bytes, req->args[2].len, &delta) != 0) {
        return resp_reply_error(ctx->out,
            "ERR value is not an integer or out of range");
    }
    /* Negating INT64_MIN would overflow; surface as the standard error. */
    if (delta == INT64_MIN) {
        return resp_reply_error(ctx->out,
            "ERR increment or decrement would overflow");
    }
    return incrby_op(ctx, &req->args[1], -delta);
}

/* ------------------------------------------------------------------------- */
/* Phase 3: string utility                                                   */
/* ------------------------------------------------------------------------- */

/* STRLEN <key> -> :N\r\n (0 if missing, matching real Redis). */
static int cmd_strlen(command_ctx_t *ctx, const resp_request_t *req)
{
    const void *val  = NULL;
    size_t      vlen = 0;
    if (ht_get(ctx->store, req->args[1].bytes, req->args[1].len, &val, &vlen)) {
        return resp_reply_integer(ctx->out, (int64_t)vlen);
    }
    return resp_reply_integer(ctx->out, 0);
}

/* APPEND <key> <value> -> :N\r\n  (N = new total length)
 *
 * If the key doesn't exist this is equivalent to SET-and-report-length.
 * Otherwise we allocate a temporary buffer holding old||new, store it,
 * and free the temp before returning. */
static int cmd_append(command_ctx_t *ctx, const resp_request_t *req)
{
    const resp_slice_t *key  = &req->args[1];
    const resp_slice_t *val  = &req->args[2];
    const void         *old  = NULL;
    size_t              olen = 0;

    if (!ht_get(ctx->store, key->bytes, key->len, &old, &olen)) {
        /* Fresh key: same shape as SET, then report length. */
        if (ht_set(ctx->store, key->bytes, key->len, val->bytes, val->len) != 0) {
            return resp_reply_error(ctx->out, "ERR out of memory");
        }
        return resp_reply_integer(ctx->out, (int64_t)val->len);
    }

    /* size_t-overflow check: olen + vlen must not wrap. */
    if (val->len > SIZE_MAX - olen) {
        return resp_reply_error(ctx->out,
            "ERR string exceeds maximum allowed size");
    }
    size_t newlen = olen + val->len;

    /* Temp buffer holds the concatenation. Freed before return on
     * every path -- success or error -- so this is one matched
     * malloc/free pair. */
    char *tmp = (char *)malloc(newlen == 0 ? 1u : newlen);
    if (tmp == NULL) {
        return resp_reply_error(ctx->out, "ERR out of memory");
    }
    if (olen > 0) {
        memcpy(tmp, old, olen);
    }
    if (val->len > 0) {
        memcpy(tmp + olen, val->bytes, val->len);
    }

    int rc;
    if (ht_set(ctx->store, key->bytes, key->len, tmp, newlen) != 0) {
        rc = resp_reply_error(ctx->out, "ERR out of memory");
    } else {
        rc = resp_reply_integer(ctx->out, (int64_t)newlen);
    }
    free(tmp);                /* matches malloc above */
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Phase 3: keyspace                                                         */
/* ------------------------------------------------------------------------- */

/* Shared by the two cmd_keys scan passes. */
typedef struct {
    bytebuf_t  *out;
    const char *pattern;
    size_t      plen;
    size_t      matched;   /* count (phase 0) / not used (phase 1)   */
    int         write;     /* 0 = count, 1 = emit bulk replies       */
    int         emit_failed; /* set if a bytebuf append fails        */
} keys_ctx_t;

static int keys_cb(const void *key, size_t klen,
                   const void *val, size_t vlen, void *ud)
{
    (void)val;
    (void)vlen;
    keys_ctx_t *kc = (keys_ctx_t *)ud;
    if (!glob_match(kc->pattern, kc->plen, (const unsigned char *)key, klen)) {
        return 1;   /* skip, keep scanning */
    }
    if (kc->write) {
        if (resp_reply_bulk(kc->out, key, klen) != 0) {
            kc->emit_failed = 1;
            return 0;   /* stop scan early */
        }
    } else {
        kc->matched++;
    }
    return 1;
}

/* KEYS <pattern> -> *N\r\n + N bulk strings.
 *
 * Two-pass: first count matches, write the array header, then emit
 * the bulks. Single-threaded server means the table can't mutate
 * between passes, so the second pass sees exactly the same matches. */
static int cmd_keys(command_ctx_t *ctx, const resp_request_t *req)
{
    keys_ctx_t kc;
    kc.out         = ctx->out;
    kc.pattern     = (const char *)req->args[1].bytes;
    kc.plen        = req->args[1].len;
    kc.matched     = 0;
    kc.write       = 0;
    kc.emit_failed = 0;

    /* Pass 1: count. */
    ht_scan(ctx->store, keys_cb, &kc);

    if (resp_reply_array_header(ctx->out, kc.matched) != 0) {
        return -1;
    }

    /* Pass 2: emit each matching key as a bulk. */
    kc.write = 1;
    ht_scan(ctx->store, keys_cb, &kc);

    return kc.emit_failed ? -1 : 0;
}

/* DBSIZE -> :N\r\n (using the count maintained by ht_set/ht_del). */
static int cmd_dbsize(command_ctx_t *ctx, const resp_request_t *req)
{
    (void)req;
    return resp_reply_integer(ctx->out, (int64_t)ctx->store->count);
}

/* FLUSHDB -> +OK\r\n
 *
 * We piggyback on ht_destroy (frees every entry) + ht_init (re-zeroes
 * the buckets). The hashtable struct itself stays put -- ht_destroy
 * is documented to leave the struct re-usable. */
static int cmd_flushdb(command_ctx_t *ctx, const resp_request_t *req)
{
    (void)req;
    ht_destroy(ctx->store);
    ht_init(ctx->store);
    return resp_reply_simple_string(ctx->out, "OK");
}

/* ------------------------------------------------------------------------- */
/* Phase 3: atomic variants                                                  */
/* ------------------------------------------------------------------------- */

/* SETNX <key> <value> -> :1 if newly stored, :0 if key already existed.
 *
 * Safe two-step (ht_get probe, then ht_set) under the single-threaded
 * model: no other request can interleave. */
static int cmd_setnx(command_ctx_t *ctx, const resp_request_t *req)
{
    if (ht_get(ctx->store, req->args[1].bytes, req->args[1].len, NULL, NULL)) {
        return resp_reply_integer(ctx->out, 0);
    }
    if (ht_set(ctx->store,
               req->args[1].bytes, req->args[1].len,
               req->args[2].bytes, req->args[2].len) != 0) {
        return resp_reply_error(ctx->out, "ERR out of memory");
    }
    return resp_reply_integer(ctx->out, 1);
}

/* GETSET <key> <value> -> old bulk reply or nil, then store new.
 *
 * The old value is borrowed from the hashtable, so we must SNAPSHOT
 * it before ht_set frees the original entry's value buffer. Snapshot
 * is freed before return on every path. */
static int cmd_getset(command_ctx_t *ctx, const resp_request_t *req)
{
    const resp_slice_t *key  = &req->args[1];
    const resp_slice_t *val  = &req->args[2];
    const void         *old  = NULL;
    size_t              olen = 0;
    int                 hit  = ht_get(ctx->store, key->bytes, key->len,
                                      &old, &olen);

    char *snapshot = NULL;
    if (hit && olen > 0) {
        snapshot = (char *)malloc(olen);
        if (snapshot == NULL) {
            return resp_reply_error(ctx->out, "ERR out of memory");
        }
        memcpy(snapshot, old, olen);
    }

    /* Now safe to overwrite. */
    if (ht_set(ctx->store, key->bytes, key->len, val->bytes, val->len) != 0) {
        free(snapshot);
        return resp_reply_error(ctx->out, "ERR out of memory");
    }

    int rc;
    if (hit) {
        /* Empty old value -> $0\r\n\r\n with a zero-length bytes
         * pointer (resp_reply_bulk handles len==0 fine). */
        rc = resp_reply_bulk(ctx->out, snapshot, olen);
    } else {
        rc = resp_reply_nil(ctx->out);
    }
    free(snapshot);          /* matches malloc above */
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Dispatch table                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *name;        /* uppercase, NUL-terminated   */
    int         min_argc;    /* including the command name  */
    int         max_argc;    /* -1 = unbounded              */
    int       (*handler)(command_ctx_t *, const resp_request_t *);
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    /* Phase 2 */
    { "PING",    1,  2, cmd_ping    },
    { "GET",     2,  2, cmd_get     },
    { "SET",     3,  3, cmd_set     },
    { "DEL",     2, -1, cmd_del     },
    { "EXISTS",  2, -1, cmd_exists  },
    { "COMMAND", 1, -1, cmd_command },
    /* Phase 3: integer arithmetic */
    { "INCR",    2,  2, cmd_incr    },
    { "DECR",    2,  2, cmd_decr    },
    { "INCRBY",  3,  3, cmd_incrby  },
    { "DECRBY",  3,  3, cmd_decrby  },
    /* Phase 3: string utility */
    { "STRLEN",  2,  2, cmd_strlen  },
    { "APPEND",  3,  3, cmd_append  },
    /* Phase 3: keyspace */
    { "KEYS",    2,  2, cmd_keys    },
    { "DBSIZE",  1,  1, cmd_dbsize  },
    { "FLUSHDB", 1,  1, cmd_flushdb },
    /* Phase 3: atomic variants */
    { "SETNX",   3,  3, cmd_setnx   },
    { "GETSET",  3,  3, cmd_getset  },
    { NULL,      0,  0, NULL        }
};

/* ------------------------------------------------------------------------- */
/* Public entry point                                                        */
/* ------------------------------------------------------------------------- */

int command_execute(command_ctx_t *ctx, const resp_request_t *req)
{
    if (req->argc == 0) {
        /* Shouldn't happen -- the parser rejects empty arrays/lines --
         * but defend in depth. */
        return resp_reply_error(ctx->out, "ERR empty command");
    }

    for (const cmd_entry_t *e = commands; e->name != NULL; ++e) {
        if (!slice_eq_ci(&req->args[0], e->name)) {
            continue;
        }

        /* Arity check. We compare via int because min/max may be -1
         * (unbounded) which size_t can't represent. argc is bounded
         * above by RESP_MAX_ARGC (32) so the cast is safe. */
        int argc = (int)req->argc;
        if (argc < e->min_argc ||
            (e->max_argc != -1 && argc > e->max_argc)) {
            char buf[128];
            snprintf(buf, sizeof buf,
                     "ERR wrong number of arguments for '%s'",
                     e->name);
            return resp_reply_error(ctx->out, buf);
        }
        return e->handler(ctx, req);
    }

    /* Unknown command. We echo the offending name into the error
     * message, but cap the printable length to 64 bytes so a hostile
     * client can't blow up snprintf. An embedded NUL in args[0]
     * truncates the printed form, which is fine. */
    char   buf[128];
    size_t printable = (req->args[0].len < 64u) ? req->args[0].len : 64u;
    snprintf(buf, sizeof buf,
             "ERR unknown command '%.*s'",
             (int)printable,
             (const char *)req->args[0].bytes);
    return resp_reply_error(ctx->out, buf);
}
