/*
 * resp.c - RESP2 request parser implementation.
 *
 * Design notes
 * ------------
 *  - Stateless: each call to resp_parse_request() looks at the buffer
 *    head from scratch. There is no per-connection parser state. This
 *    trades a little CPU on partial frames (we re-scan previously-seen
 *    bytes on retry) for a much simpler API and zero allocations.
 *
 *  - Strict on "untouched on retry": both parse_multi_bulk() and
 *    parse_inline() stage results in a stack-local args[] array first
 *    and copy into *out only on RESP_OK. So a caller passing the same
 *    `resp_request_t` across multiple NEED_MORE retries is fine.
 *
 *  - No malloc(), no free(), nothing to document in an allocation table.
 *    Slices in `out->args[]` are borrowed pointers into the caller's
 *    input buffer.
 */

#include "resp.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Length / number helpers                                                   */
/* ------------------------------------------------------------------------- */

/* Parse an ASCII signed integer terminated by \r\n at the head of `p`.
 *
 *   p, avail        : the buffer slice to read from.
 *   *value_out      : on RESP_OK, receives the parsed value.
 *   *consumed_out   : on RESP_OK, receives the byte count (digits + CRLF).
 *
 * Returns RESP_OK on a full valid number, RESP_NEED_MORE if the buffer
 * ends mid-number (no terminating \r\n yet), or RESP_PROTOCOL_ERR on
 * non-digit input, missing digits, or 64-bit overflow.
 *
 * Strictness notes:
 *   - At least one digit is required ("-\r\n" and "\r\n" are errors).
 *   - "+123" is rejected (Redis writes lengths without a leading '+').
 *   - Leading zeros are tolerated ("007" -> 7); real Redis allows this. */
static resp_status_t parse_int_until_crlf(const unsigned char *p,
                                          size_t               avail,
                                          int64_t             *value_out,
                                          size_t              *consumed_out)
{
    int64_t value      = 0;
    int     negative   = 0;
    size_t  i          = 0;
    size_t  digits_at  = 0;

    if (i < avail && p[i] == '-') {
        negative  = 1;
        i++;
        digits_at = i;
    }

    while (i < avail) {
        unsigned char c = p[i];

        if (c == '\r') {
            /* Need the matching \n before we can commit. */
            if (i + 1 >= avail) {
                return RESP_NEED_MORE;
            }
            if (p[i + 1] != '\n') {
                return RESP_PROTOCOL_ERR;
            }
            if (i == digits_at) {
                /* No actual digits between optional '-' and \r\n. */
                return RESP_PROTOCOL_ERR;
            }
            *value_out    = negative ? -value : value;
            *consumed_out = i + 2;   /* digits + \r\n */
            return RESP_OK;
        }

        if (c < '0' || c > '9') {
            return RESP_PROTOCOL_ERR;
        }

        /* Overflow guard: value*10 + 9 must still fit in int64_t. */
        if (value > (INT64_MAX - 9) / 10) {
            return RESP_PROTOCOL_ERR;
        }
        value = value * 10 + (int64_t)(c - '0');
        i++;
    }

    /* Ran out of bytes mid-number or before the CRLF. */
    return RESP_NEED_MORE;
}

/* ------------------------------------------------------------------------- */
/* Bulk string                                                               */
/* ------------------------------------------------------------------------- */

/* Parse a "$L\r\n<L bytes>\r\n" bulk string at the head of `p`. */
static resp_status_t parse_bulk(const unsigned char *p,
                                size_t               avail,
                                resp_slice_t        *slice_out,
                                size_t              *consumed_out)
{
    if (avail == 0) {
        return RESP_NEED_MORE;
    }
    if (p[0] != '$') {
        return RESP_PROTOCOL_ERR;
    }

    int64_t       length;
    size_t        hdr_used;
    resp_status_t st = parse_int_until_crlf(p + 1, avail - 1, &length, &hdr_used);
    if (st != RESP_OK) {
        return st;
    }

    /* Negative length ("$-1\r\n" / nil) is legal in REPLIES but never
     * appears in a client REQUEST -- reject it here. Likewise enforce
     * the 512 MiB cap before we trust the length for arithmetic. */
    if (length < 0 || (uint64_t)length > (uint64_t)RESP_MAX_BULK_LEN) {
        return RESP_PROTOCOL_ERR;
    }

    size_t header_size = 1u + hdr_used;                  /* '$' + digits + \r\n  */
    size_t body_size   = (size_t)length;
    size_t total       = header_size + body_size + 2u;   /* + trailing \r\n      */

    /* Overflow check: total must not wrap (it could if length were
     * huge, though our cap above already prevents that on 64-bit). */
    if (total < header_size) {
        return RESP_PROTOCOL_ERR;
    }
    if (avail < total) {
        return RESP_NEED_MORE;
    }

    /* The bulk body MUST be followed by exactly \r\n. */
    if (p[header_size + body_size]     != '\r' ||
        p[header_size + body_size + 1] != '\n') {
        return RESP_PROTOCOL_ERR;
    }

    slice_out->bytes = p + header_size;
    slice_out->len   = body_size;
    *consumed_out    = total;
    return RESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Multi-bulk array (the redis-cli wire form)                                */
/* ------------------------------------------------------------------------- */

/* Caller guarantees input[0] == '*'. */
static resp_status_t parse_multi_bulk(const unsigned char *input,
                                      size_t               len,
                                      resp_request_t      *out)
{
    int64_t       argc;
    size_t        hdr_used;
    resp_status_t st = parse_int_until_crlf(input + 1, len - 1, &argc, &hdr_used);
    if (st != RESP_OK) {
        return st;
    }

    /* Reject empty arrays, nil arrays, and over-large argv. We require
     * at least one element because RESP requests are always (cmd, args...). */
    if (argc < 1 || (uint64_t)argc > (uint64_t)RESP_MAX_ARGC) {
        return RESP_PROTOCOL_ERR;
    }
    /* After the range check we can narrow to size_t safely; doing it
     * once here keeps the rest of the function free of signed/unsigned
     * conversion noise. */
    size_t expected_argc = (size_t)argc;

    /* Stage into stack-locals so *out is untouched on NEED_MORE. */
    resp_slice_t tmp[RESP_MAX_ARGC];
    size_t       pos = 1u + hdr_used;

    for (size_t i = 0; i < expected_argc; ++i) {
        size_t bulk_used;
        st = parse_bulk(input + pos, len - pos, &tmp[i], &bulk_used);
        if (st != RESP_OK) {
            return st;   /* NEED_MORE or PROTOCOL_ERR -- propagate */
        }
        pos += bulk_used;
    }

    /* All elements parsed: now commit to *out. */
    for (size_t i = 0; i < expected_argc; ++i) {
        out->args[i] = tmp[i];
    }
    out->argc           = expected_argc;
    out->bytes_consumed = pos;
    return RESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Inline command                                                            */
/* ------------------------------------------------------------------------- */

/* Parse a line-terminated `CMD arg1 arg2\r\n` (or just `\n`) into argv. */
static resp_status_t parse_inline(const unsigned char *input,
                                  size_t               len,
                                  resp_request_t      *out)
{
    /* Bounded scan so a malicious client that never sends a newline
     * can't make us walk an unbounded buffer. */
    size_t scan_limit = (len < RESP_MAX_INLINE_LEN) ? len : RESP_MAX_INLINE_LEN;
    size_t nl         = scan_limit;   /* sentinel: "not found"            */

    for (size_t i = 0; i < scan_limit; ++i) {
        if (input[i] == '\n') {
            nl = i;
            break;
        }
    }

    if (nl == scan_limit) {
        /* No newline yet. If we already hit the cap, the line is
         * pathologically long; otherwise just wait for more bytes. */
        if (len >= RESP_MAX_INLINE_LEN) {
            return RESP_PROTOCOL_ERR;
        }
        return RESP_NEED_MORE;
    }

    /* Trim an optional preceding '\r' so both \r\n and bare \n work. */
    size_t line_end = nl;
    if (line_end > 0 && input[line_end - 1] == '\r') {
        line_end -= 1;
    }

    /* Tokenize on runs of space/tab. Empty input or whitespace-only
     * input is treated as a protocol error. */
    resp_slice_t tmp[RESP_MAX_ARGC];
    size_t       argc = 0;
    size_t       i    = 0;

    while (i < line_end) {
        while (i < line_end && (input[i] == ' ' || input[i] == '\t')) {
            i++;
        }
        if (i >= line_end) {
            break;
        }
        if (argc >= RESP_MAX_ARGC) {
            return RESP_PROTOCOL_ERR;
        }

        size_t tok_start = i;
        while (i < line_end && input[i] != ' ' && input[i] != '\t') {
            i++;
        }
        tmp[argc].bytes = input + tok_start;
        tmp[argc].len   = i - tok_start;
        argc++;
    }

    if (argc == 0) {
        return RESP_PROTOCOL_ERR;
    }

    for (size_t k = 0; k < argc; ++k) {
        out->args[k] = tmp[k];
    }
    out->argc           = argc;
    out->bytes_consumed = nl + 1u;   /* include the final '\n' */
    return RESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Public entry point                                                        */
/* ------------------------------------------------------------------------- */

resp_status_t resp_parse_request(const unsigned char *input,
                                 size_t               len,
                                 resp_request_t      *out)
{
    if (len == 0) {
        return RESP_NEED_MORE;
    }

    /* The classic RESP discriminator: '*' means multi-bulk, anything
     * else is treated as an inline command. */
    if (input[0] == '*') {
        return parse_multi_bulk(input, len, out);
    }
    return parse_inline(input, len, out);
}

/* ------------------------------------------------------------------------- */
/* Reply serializers                                                         */
/* ------------------------------------------------------------------------- */

int resp_reply_simple_string(bytebuf_t *out, const char *s)
{
    if (bytebuf_append(out, "+", 1) < 0)            return -1;
    if (bytebuf_append(out, s, strlen(s)) < 0)      return -1;
    return bytebuf_append(out, "\r\n", 2);
}

int resp_reply_error(bytebuf_t *out, const char *msg)
{
    if (bytebuf_append(out, "-", 1) < 0)            return -1;
    if (bytebuf_append(out, msg, strlen(msg)) < 0)  return -1;
    return bytebuf_append(out, "\r\n", 2);
}

int resp_reply_integer(bytebuf_t *out, int64_t n)
{
    return bytebuf_appendf(out, ":%" PRId64 "\r\n", n);
}

int resp_reply_bulk(bytebuf_t *out, const void *bytes, size_t len)
{
    /* "$<len>\r\n" header, then the binary body, then a trailing CRLF.
     * Bulk strings are length-prefixed, so the body may contain any
     * bytes -- NULs and 0xff round-trip cleanly. */
    if (bytebuf_appendf(out, "$%zu\r\n", len) < 0) return -1;
    if (bytebuf_append(out, bytes, len) < 0)       return -1;
    return bytebuf_append(out, "\r\n", 2);
}

int resp_reply_nil(bytebuf_t *out)
{
    /* RESP2 nil is the special "$-1\r\n" bulk-string sentinel. */
    return bytebuf_append(out, "$-1\r\n", 5);
}

int resp_reply_array_header(bytebuf_t *out, size_t n)
{
    return bytebuf_appendf(out, "*%zu\r\n", n);
}
