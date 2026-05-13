/*
 * command.c - dispatch table + per-command handlers.
 *
 * This module is glue: it translates a parsed `resp_request_t` (from
 * resp.c) into hashtable operations (from hashtable.c) and a RESP
 * reply (also formatted via resp.c) appended to a bytebuf provided
 * by the caller. There are no allocations here -- everything either
 * borrows from the request, lives in the hashtable, or is pushed into
 * the caller's bytebuf.
 *
 * Adding a new command means: write a `cmd_xxx` static handler, then
 * add one row to the `commands[]` table at the bottom. The dispatcher
 * picks it up automatically.
 */

#include "command.h"

#include <stdio.h>
#include <stdint.h>
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
/* Dispatch table                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *name;        /* uppercase, NUL-terminated   */
    int         min_argc;    /* including the command name  */
    int         max_argc;    /* -1 = unbounded              */
    int       (*handler)(command_ctx_t *, const resp_request_t *);
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    { "PING",    1,  2, cmd_ping    },
    { "GET",     2,  2, cmd_get     },
    { "SET",     3,  3, cmd_set     },
    { "DEL",     2, -1, cmd_del     },
    { "EXISTS",  2, -1, cmd_exists  },
    { "COMMAND", 1, -1, cmd_command },
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
