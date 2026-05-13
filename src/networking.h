/*
 * networking.h - placeholder for the Phase 2 single-threaded event loop.
 *
 * Planned architecture (to be filled in next):
 *
 *   - One listening socket, set non-blocking via fcntl(O_NONBLOCK).
 *   - One epoll(7) instance, edge-triggered (EPOLLET) so we read/write
 *     until EAGAIN before going back to epoll_wait().
 *   - Per-connection state (read buffer, write buffer, parser cursor)
 *     kept in a heap-allocated struct attached to epoll_event.data.ptr.
 *   - Commands are decoded from the RESP protocol and dispatched against
 *     the shared `hashtable_t *store` handed in at net_init() time.
 *   - No threads, no locks: the event loop owns the store outright.
 *
 * For now both functions are stubs that just log and return success so
 * main.c can wire the call sites without a real socket on the box.
 */

#ifndef REDIS_IN_C_NETWORKING_H
#define REDIS_IN_C_NETWORKING_H

#include "hashtable.h"

/* Wire the networking subsystem to the shared key/value store.
 *
 * The pointer is borrowed for the lifetime of the loop -- networking does
 * not free it. Returns 0 on success, non-zero on setup failure (will be
 * meaningful once epoll/socket setup actually happens). */
int net_init(hashtable_t *store);

/* Run the event loop. In Phase 2 this will block on epoll_wait() until a
 * shutdown signal is received. The stub returns immediately. */
int net_run(void);

#endif /* REDIS_IN_C_NETWORKING_H */
