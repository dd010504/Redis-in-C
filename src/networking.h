/*
 * networking.h - public entry points for the single-threaded event loop.
 *
 * Current state (Phase 2 step 3)
 * ------------------------------
 *   - One listening socket on 127.0.0.1:6379, non-blocking.
 *   - One epoll(7) instance in LEVEL-TRIGGERED mode (no EPOLLET).
 *   - Per-connection state lives in a heap-allocated struct attached
 *     to epoll_event.data.ptr. The listening socket uses data.ptr ==
 *     NULL as a sentinel so dispatch doesn't need a hash lookup.
 *   - Each connection has two growable byte buffers (bytebuf_t): one
 *     for incoming bytes (parsed by resp_parse_request) and one for
 *     queued replies (filled by command_execute, drained to the
 *     socket by handle_write).
 *   - EPOLLIN is armed at all times during normal operation; EPOLLOUT
 *     is armed whenever there are pending reply bytes.
 *   - Commands implemented: PING, GET, SET, DEL, EXISTS, COMMAND.
 *     redis-cli -p 6379 talks to this server end-to-end.
 *
 * Not yet here (see roadmap)
 * --------------------------
 *   - TTL / EXPIRE (Phase 4)
 *   - INCR / DECR / KEYS / DBSIZE / FLUSHDB (Phase 3 follow-ups)
 *   - Edge-triggered epoll, CLI flags, benchmarking (Phase 6)
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
