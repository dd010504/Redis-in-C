/*
 * command.h - command dispatcher.
 *
 * Looks up the command name in args[0] (case-insensitive), checks
 * arity, and either invokes the handler or appends an appropriate
 * error reply.
 *
 * Every invocation appends exactly one RESP reply to ctx->out before
 * returning. The only failure mode is ctx->out running out of room
 * (OOM or BYTEBUF_MAX), reported as a non-zero return. In that case
 * the partial reply already in the buffer is unreliable and the
 * caller should close the connection without trying to flush it.
 */

#ifndef REDIS_IN_C_COMMAND_H
#define REDIS_IN_C_COMMAND_H

#include "bytebuf.h"
#include "hashtable.h"
#include "resp.h"

typedef struct {
    hashtable_t *store;  /* borrowed -- never freed by command_*  */
    bytebuf_t   *out;    /* borrowed -- reply gets appended here  */
} command_ctx_t;

/* Dispatch a parsed RESP request. Returns 0 on success (a reply was
 * appended), -1 if the reply could not be appended due to bytebuf
 * OOM / BYTEBUF_MAX (caller should close the connection). */
int command_execute(command_ctx_t *ctx, const resp_request_t *req);

#endif /* REDIS_IN_C_COMMAND_H */
