/*
 * resp.h - RESP2 request parser (Phase 2 step 2).
 *
 * Scope
 * -----
 *  Accepts a byte buffer and tries to extract exactly ONE command frame
 *  from the beginning of it. Two input forms are recognised:
 *
 *    1. Multi-bulk (what redis-cli sends):
 *
 *         *N\r\n
 *         $L1\r\n<L1 bytes>\r\n
 *         $L2\r\n<L2 bytes>\r\n
 *         ...
 *
 *    2. Inline (what `nc` / telnet sends):
 *
 *         CMD arg1 arg2\r\n     (or just '\n', for telnet leniency)
 *
 *  Both forms produce the same (argv[], argc, bytes_consumed) output.
 *
 * Memory contract
 * ---------------
 *  The parser is a pure function: it never allocates and never frees.
 *  The `args[i].bytes` pointers in the output are BORROWED slices into
 *  the caller's input buffer. They are valid exactly as long as the
 *  caller leaves the input bytes in place. Step 3 will be responsible
 *  for advancing the connection read buffer by `bytes_consumed` AFTER
 *  it is done using the slices.
 *
 *  On RESP_NEED_MORE and RESP_PROTOCOL_ERR the parser does NOT write to
 *  *out -- the caller can leave the result variable untouched and retry
 *  later once more bytes have been read.
 */

#ifndef REDIS_IN_C_RESP_H
#define REDIS_IN_C_RESP_H

#include "bytebuf.h"

#include <stddef.h>
#include <stdint.h>

/* Hard cap on the number of arguments in one command. Real Redis goes
 * far higher, but commands with > 32 args are rare and capping cheaply
 * defends against argv-bomb-style abuse from misbehaving clients. */
#define RESP_MAX_ARGC       32

/* Match real Redis' 512 MiB cap on bulk string length so we reject
 * obviously-malicious length headers before allocating anything. */
#define RESP_MAX_BULK_LEN   (512u * 1024u * 1024u)

/* Cap on a single inline command line. 8 KiB is plenty for any
 * realistic telnet-style command and stops a client from making us
 * scan forever for a terminating newline. */
#define RESP_MAX_INLINE_LEN (8u * 1024u)

/* A borrowed view into the caller's input buffer. NOT NUL-terminated;
 * use the explicit `len` for every byte access. */
typedef struct {
    const unsigned char *bytes;
    size_t               len;
} resp_slice_t;

/* Parser outcome.
 *
 *   RESP_OK            -- one full frame parsed; *out is valid and
 *                         bytes_consumed tells you how far to advance.
 *   RESP_NEED_MORE     -- buffer is a valid prefix of a frame but not
 *                         yet complete. *out is untouched. Caller should
 *                         read more bytes and retry from the same offset.
 *   RESP_PROTOCOL_ERR  -- input is unambiguously malformed. *out is
 *                         untouched. Caller should send a protocol-error
 *                         reply and close the connection. */
typedef enum {
    RESP_OK           =  0,
    RESP_NEED_MORE    =  1,
    RESP_PROTOCOL_ERR = -1
} resp_status_t;

/* Output of one successful parse. `args[0]` is the command name (e.g.
 * "SET"), `args[1..argc-1]` are its arguments. */
typedef struct {
    resp_slice_t args[RESP_MAX_ARGC];
    size_t       argc;
    size_t       bytes_consumed;
} resp_request_t;

/* Try to extract one command from input[0 .. len). See the comments
 * above for behaviour and the memory contract on each status code. */
resp_status_t resp_parse_request(const unsigned char *input,
                                 size_t               len,
                                 resp_request_t      *out);

/* -------------------------------------------------------------------------
 * Reply serializers (the write side of the wire protocol).
 *
 * Each function appends one RESP-encoded reply to `out`. They return
 * 0 on success and -1 if the underlying bytebuf cannot grow (OOM or
 * BYTEBUF_MAX exceeded) -- in the failure case the bytebuf is left in
 * a valid but possibly partially-written state, so callers that hit
 * an error should typically close the connection rather than try to
 * recover.
 * ------------------------------------------------------------------------- */

/* "+<s>\r\n" -- simple status string, e.g. "OK", "PONG". */
int resp_reply_simple_string(bytebuf_t *out, const char *s);

/* "-<msg>\r\n" -- error string (caller includes any prefix like "ERR "). */
int resp_reply_error(bytebuf_t *out, const char *msg);

/* ":<n>\r\n" -- 64-bit signed integer. */
int resp_reply_integer(bytebuf_t *out, int64_t n);

/* "$<len>\r\n<bytes>\r\n" -- binary-safe bulk string. */
int resp_reply_bulk(bytebuf_t *out, const void *bytes, size_t len);

/* "$-1\r\n" -- the nil bulk reply used for cache misses. */
int resp_reply_nil(bytebuf_t *out);

/* "*<n>\r\n" -- array header only. Caller appends the N elements via
 * the other serializers immediately after. */
int resp_reply_array_header(bytebuf_t *out, size_t n);

#endif /* REDIS_IN_C_RESP_H */
