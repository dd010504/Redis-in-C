/*
 * networking.h - public entry points for the single-threaded event loop.
 *
 * Current state (Phase 2 step 1)
 * ------------------------------
 *   - One listening socket on 127.0.0.1:6379, non-blocking.
 *   - One epoll(7) instance running in LEVEL-TRIGGERED mode (no EPOLLET).
 *     Level-triggered means the kernel re-notifies us until the condition
 *     is gone, so we only do one read() / write() per event instead of
 *     looping until EAGAIN. Easier to reason about while learning; we can
 *     switch to edge-triggered later if we need the throughput.
 *   - Per-connection state lives in a heap-allocated struct attached to
 *     epoll_event.data.ptr. Listening socket uses data.ptr == NULL as a
 *     sentinel so we can dispatch without a hash lookup.
 *   - Today the bytes a client sends are simply echoed back. The hashtable
 *     pointer handed to net_init() is stashed for step 2 but not yet read.
 *
 * Next steps (not yet implemented)
 * --------------------------------
 *   - Step 2: parse RESP frames (arrays of bulk strings, plus inline
 *     commands) out of the per-connection read buffer.
 *   - Step 3: dispatch GET / SET / DEL / PING against the shared
 *     hashtable_t and write a RESP reply back to the client.
 */

#ifndef REDIS_IN_C_NETWORKING_H
#define REDIS_IN_C_NETWORKING_H

#include "hashtable.h"

/* Wire the networking subsystem to the shared key/value store, create the
 * listening socket, build the epoll instance, and install SIGINT/SIGTERM
 * handlers.
 *
 * The `store` pointer is borrowed for the lifetime of the loop; networking
 * does NOT free it. Returns 0 on success, -1 on setup failure (in which
 * case any partially-opened fds have already been cleaned up). */
int net_init(hashtable_t *store);

/* Run the event loop until SIGINT/SIGTERM. Performs full teardown of every
 * live connection, the epoll fd, and the listening fd before returning, so
 * a clean shutdown leaves no heap allocations live. Returns 0 on normal
 * shutdown, non-zero on a fatal epoll error. */
int net_run(void);

#endif /* REDIS_IN_C_NETWORKING_H */
