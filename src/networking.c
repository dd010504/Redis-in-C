/*
 * networking.c - Phase 2 step 3: single-threaded, level-triggered epoll
 *                event loop with full RESP command dispatch.
 *
 * Design summary
 * --------------
 *   - One listening socket on 127.0.0.1:6379 (loopback only; flip to
 *     INADDR_ANY later when the protocol layer is hardened).
 *   - Non-blocking sockets everywhere; the loop drives all I/O.
 *   - epoll(7) in LEVEL-TRIGGERED mode (no EPOLLET). The kernel keeps
 *     re-notifying us until the condition is gone, which lets us read
 *     and write in straightforward loops without strict EAGAIN-state
 *     bookkeeping.
 *   - Per-connection state (conn_t) is heap-allocated and pointed to
 *     by epoll_event.data.ptr. The listening socket uses data.ptr ==
 *     NULL as a sentinel so dispatch doesn't need a hash lookup.
 *   - g_conns is a singly-linked list of every live conn_t so that on
 *     SIGINT we can close + free every connection cleanly. Keeps the
 *     server valgrind-clean across Ctrl-C.
 *
 * Read/dispatch/write flow (no half-duplex toggle anymore)
 * --------------------------------------------------------
 *   handle_read():
 *     1. read() into in_buf.tail until EAGAIN, EOF, or BYTEBUF_MAX.
 *     2. Loop: resp_parse_request -> command_execute (which appends a
 *        reply to out_buf) -> bytebuf_consume. Stops on NEED_MORE
 *        (wait for more bytes) or PROTOCOL_ERR (queue an error reply
 *        and mark the connection for closure once out_buf drains).
 *     3. If a read error was deferred (e.g. BYTEBUF_MAX hit), append
 *        the error AFTER any successful replies so ordering is sane.
 *     4. Adjust interest: EPOLLIN if still accepting more input;
 *        EPOLLOUT if any unsent reply remains.
 *
 *   handle_write():
 *     1. write() from out_buf.head until EAGAIN or drained.
 *     2. On drain: close if close_after_drain is set; else disarm
 *        EPOLLOUT and keep EPOLLIN armed for the next request.
 *
 * Allocation summary (every malloc has a matching free pinned below):
 *
 *   site                              allocates             freed by
 *   ------------------------------    -------------------   ----------------
 *   accept_clients (new client)       one conn_t            close_conn / shutdown
 *                                                           teardown in net_run
 *   bytebuf growth inside conn->in_buf  backing byte array  bytebuf_free
 *   bytebuf growth inside conn->out_buf backing byte array  bytebuf_free
 *
 *   close_conn calls bytebuf_free on both buffers BEFORE free()-ing
 *   the conn_t, so the byte arrays never outlive the struct.
 */

#define _GNU_SOURCE  /* for accept4(), if available -- harmless otherwise */

#include "networking.h"

#include "bytebuf.h"
#include "command.h"
#include "resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define LISTEN_HOST   "127.0.0.1"
#define LISTEN_PORT   6379
#define MAX_EVENTS    64           /* epoll_wait batch size                 */
#define READ_CHUNK    4096         /* per-iteration read() request size     */

/* ------------------------------------------------------------------------- */
/* Per-connection state                                                      */
/* ------------------------------------------------------------------------- */

typedef struct conn {
    int          fd;
    bytebuf_t    in_buf;             /* bytes read from peer, fed to parser  */
    bytebuf_t    out_buf;            /* serialized replies awaiting write    */
    uint32_t     events;             /* current epoll interest mask          */
    int          close_after_drain;  /* close once out_buf is fully written  */
    struct conn *next;               /* link in g_conns                      */
} conn_t;

/* ------------------------------------------------------------------------- */
/* Module-private state                                                      */
/* ------------------------------------------------------------------------- */

static hashtable_t            *g_store     = NULL;   /* borrowed for command dispatch */
static int                     g_listen_fd = -1;
static int                     g_epoll_fd  = -1;
static conn_t                 *g_conns     = NULL;
static volatile sig_atomic_t   g_shutdown  = 0;

/* ------------------------------------------------------------------------- */
/* Forward declarations                                                      */
/* ------------------------------------------------------------------------- */

static int  setup_listener(uint16_t port);
static int  set_nonblock(int fd);

static void accept_clients(void);
static int  handle_read(conn_t *c);
static int  handle_write(conn_t *c);
static int  arm_events(conn_t *c, uint32_t events);

static void close_conn(conn_t *c);
static void on_signal(int sig);

/* ------------------------------------------------------------------------- */
/* Socket helpers                                                            */
/* ------------------------------------------------------------------------- */

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL O_NONBLOCK)");
        return -1;
    }
    return 0;
}

static int setup_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* SO_REUSEADDR so we can rebind immediately after a crash/exit. */
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    if (set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, LISTEN_HOST, &addr.sin_addr) != 1) {
        fprintf(stderr, "[networking] invalid host: %s\n", LISTEN_HOST);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------------- */
/* epoll interest helpers                                                    */
/* ------------------------------------------------------------------------- */

static int arm_events(conn_t *c, uint32_t events)
{
    /* No-op fast path when the interest mask isn't actually changing. */
    if (c->events == events) {
        return 0;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = events;
    ev.data.ptr = c;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
        perror("epoll_ctl(MOD)");
        return -1;
    }
    c->events = events;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Connection lifecycle                                                      */
/* ------------------------------------------------------------------------- */

static void accept_clients(void)
{
    /* Drain accept until EAGAIN -- one syscall storm per epoll wake. */
    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof peer;
        int                cfd      = accept(g_listen_fd,
                                             (struct sockaddr *)&peer,
                                             &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return;
        }

        if (set_nonblock(cfd) < 0) {
            close(cfd);
            continue;
        }

        /* Allocate per-connection state. Freed in close_conn(). */
        conn_t *c = (conn_t *)malloc(sizeof(*c));
        if (c == NULL) {
            fprintf(stderr, "[networking] OOM allocating conn for fd=%d\n", cfd);
            close(cfd);
            continue;
        }
        c->fd                = cfd;
        c->events            = 0;
        c->close_after_drain = 0;
        bytebuf_init(&c->in_buf);
        bytebuf_init(&c->out_buf);
        c->next = g_conns;
        g_conns = c;

        /* Register with epoll. Start in "wants to read" mode -- the
         * out_buf is empty so EPOLLOUT would just busy-wake us. */
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events   = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            perror("epoll_ctl(ADD)");
            /* Unlink + free everything we just allocated. */
            g_conns = c->next;
            bytebuf_free(&c->in_buf);
            bytebuf_free(&c->out_buf);
            free(c);
            close(cfd);
            continue;
        }
        c->events = EPOLLIN;

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        fprintf(stderr, "[networking] accepted fd=%d from %s:%u\n",
                cfd, ip, (unsigned)ntohs(peer.sin_port));
    }
}

static void close_conn(conn_t *c)
{
    /* Explicit DEL before close() to be tidy; the kernel removes the
     * fd on close anyway, but being explicit keeps us safe against
     * dup()/fork() surprises later. */
    if (g_epoll_fd >= 0) {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    }
    int saved_fd = c->fd;
    close(c->fd);

    /* Unlink from g_conns. */
    conn_t **link = &g_conns;
    while (*link != NULL && *link != c) {
        link = &(*link)->next;
    }
    if (*link == c) {
        *link = c->next;
    }

    bytebuf_free(&c->in_buf);    /* releases growth allocations */
    bytebuf_free(&c->out_buf);

    fprintf(stderr, "[networking] closed fd=%d\n", saved_fd);
    free(c);   /* matches the malloc in accept_clients() */
}

/* ------------------------------------------------------------------------- */
/* I/O handlers                                                              */
/* ------------------------------------------------------------------------- */

static int handle_read(conn_t *c)
{
    /* If we're already in shutdown-after-drain mode, just adjust
     * interest and let handle_write finish the job. */
    if (c->close_after_drain) {
        if (bytebuf_len(&c->out_buf) == 0) {
            return -1;
        }
        return arm_events(c, EPOLLOUT);
    }

    /* Drain socket into in_buf until EAGAIN or a soft error. We
     * defer surfacing the error until AFTER dispatching any
     * already-complete frames, so the error reply lands at the end
     * of out_buf rather than ahead of legitimate replies. */
    int         read_failed  = 0;
    const char *read_err_msg = NULL;

    for (;;) {
        if (bytebuf_len(&c->in_buf) >= BYTEBUF_MAX) {
            read_failed  = 1;
            read_err_msg = "ERR request too large";
            break;
        }

        unsigned char *tail = bytebuf_reserve(&c->in_buf, READ_CHUNK);
        if (tail == NULL) {
            read_failed  = 1;
            read_err_msg = "ERR out of memory";
            break;
        }

        ssize_t n = read(c->fd, tail, READ_CHUNK);
        if (n == 0) {
            return -1;   /* peer closed -- caller will close_conn() */
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            return -1;
        }
        bytebuf_advance(&c->in_buf, (size_t)n);
    }

    /* Dispatch every complete RESP frame currently in in_buf. */
    command_ctx_t ctx = { .store = g_store, .out = &c->out_buf };
    for (;;) {
        resp_request_t req;
        resp_status_t  st = resp_parse_request(bytebuf_data(&c->in_buf),
                                               bytebuf_len(&c->in_buf),
                                               &req);
        if (st == RESP_NEED_MORE) {
            break;
        }
        if (st == RESP_PROTOCOL_ERR) {
            resp_reply_error(&c->out_buf, "ERR Protocol error");
            c->close_after_drain = 1;
            break;
        }
        /* RESP_OK */
        if (command_execute(&ctx, &req) != 0) {
            /* Reply formatting OOM -- can't continue cleanly. */
            return -1;
        }
        bytebuf_consume(&c->in_buf, req.bytes_consumed);
    }

    /* Now (and only now) queue the read-side error if any. */
    if (read_failed) {
        resp_reply_error(&c->out_buf, read_err_msg);
        c->close_after_drain = 1;
    }

    /* Nothing left to send and we're shutting down -- close now. */
    if (c->close_after_drain && bytebuf_len(&c->out_buf) == 0) {
        return -1;
    }

    /* Adjust interest: EPOLLIN unless we're winding down, EPOLLOUT
     * iff there's unsent data. */
    uint32_t want = 0;
    if (!c->close_after_drain) {
        want |= EPOLLIN;
    }
    if (bytebuf_len(&c->out_buf) > 0) {
        want |= EPOLLOUT;
    }
    if (want == 0) {
        return -1;   /* defensive: nothing to wait on -> close */
    }
    return arm_events(c, want);
}

static int handle_write(conn_t *c)
{
    /* Try to fully drain out_buf in one go. */
    while (bytebuf_len(&c->out_buf) > 0) {
        ssize_t n = write(c->fd,
                          bytebuf_data(&c->out_buf),
                          bytebuf_len(&c->out_buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;   /* kernel buffer full; retry on next EPOLLOUT */
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return -1;  /* peer gone -- close */
            }
            perror("write");
            return -1;
        }
        bytebuf_consume(&c->out_buf, (size_t)n);
    }

    /* Drained. If we were waiting to close after this final flush, do it. */
    if (c->close_after_drain) {
        return -1;
    }

    /* Otherwise just disarm EPOLLOUT and wait for the next request. */
    return arm_events(c, EPOLLIN);
}

/* ------------------------------------------------------------------------- */
/* Signal handling                                                           */
/* ------------------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    /* Only a sig_atomic_t store is async-signal-safe here. epoll_wait
     * will return EINTR and the main loop checks g_shutdown. */
    g_shutdown = 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int net_init(hashtable_t *store)
{
    g_store = store;   /* borrowed; commands dispatch against it */

    g_listen_fd = setup_listener(LISTEN_PORT);
    if (g_listen_fd < 0) {
        return -1;
    }

    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        perror("epoll_create1");
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    /* Register the listening socket. data.ptr == NULL is our sentinel
     * for "this event is on the listener". */
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
        perror("epoll_ctl(ADD listener)");
        close(g_epoll_fd);
        close(g_listen_fd);
        g_epoll_fd  = -1;
        g_listen_fd = -1;
        return -1;
    }

    /* Install signal handlers for graceful shutdown. Deliberately not
     * using SA_RESTART -- we want epoll_wait to return EINTR so the
     * loop checks g_shutdown promptly. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGPIPE would kill us if a peer closes mid-write. We surface
     * EPIPE from write() ourselves, so ignore the signal. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[networking] listening on %s:%d (epoll level-triggered)\n",
            LISTEN_HOST, LISTEN_PORT);
    return 0;
}

int net_run(void)
{
    struct epoll_event events[MAX_EVENTS];

    while (!g_shutdown) {
        int n = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                /* Likely SIGINT/SIGTERM -- re-check the flag and loop. */
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            uint32_t ev = events[i].events;

            /* Listening socket: accept everything pending. */
            if (events[i].data.ptr == NULL) {
                if (ev & (EPOLLERR | EPOLLHUP)) {
                    fprintf(stderr, "[networking] listener error -- shutting down\n");
                    g_shutdown = 1;
                    break;
                }
                accept_clients();
                continue;
            }

            /* Client socket. */
            conn_t *c = (conn_t *)events[i].data.ptr;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                /* Could still have buffered replies; just close. */
                close_conn(c);
                continue;
            }
            if (ev & EPOLLIN) {
                if (handle_read(c) < 0) {
                    close_conn(c);
                    continue;
                }
            }
            if (ev & EPOLLOUT) {
                if (handle_write(c) < 0) {
                    close_conn(c);
                    continue;
                }
            }
        }
    }

    /* Teardown: free every live conn, then the epoll/listen fds. */
    fprintf(stderr, "[networking] shutting down...\n");
    while (g_conns != NULL) {
        conn_t *next = g_conns->next;
        if (g_epoll_fd >= 0) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, g_conns->fd, NULL);
        }
        close(g_conns->fd);
        bytebuf_free(&g_conns->in_buf);
        bytebuf_free(&g_conns->out_buf);
        free(g_conns);   /* matches the malloc in accept_clients() */
        g_conns = next;
    }
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    return 0;
}
