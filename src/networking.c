/*
 * networking.c - Phase 2 step 1: single-threaded, level-triggered epoll
 *                event loop. For now it just echoes received bytes back
 *                to the client. RESP framing and hashtable dispatch land
 *                in step 2.
 *
 * Design summary
 * --------------
 *   - One listening socket on 127.0.0.1:6379 (loopback only; flip to
 *     INADDR_ANY later when the protocol layer is hardened).
 *   - Non-blocking sockets everywhere; the loop drives all I/O.
 *   - epoll(7) in LEVEL-TRIGGERED mode (no EPOLLET). That means the
 *     kernel keeps re-notifying us until the condition is gone, so we
 *     only need one read() / write() per event instead of looping to
 *     EAGAIN. Easier to reason about while we're learning.
 *   - Per-connection state lives in a heap-allocated conn_t pointed to
 *     by epoll_event.data.ptr. The listening socket uses data.ptr ==
 *     NULL as a sentinel so we can tell the two apart without a hash.
 *   - We keep a singly-linked list (g_conns) of every active conn_t so
 *     that on SIGINT we can close+free every connection cleanly. This
 *     keeps the server valgrind-clean across Ctrl-C.
 *
 * Echo flow (half-duplex, fixed 4 KiB buffer)
 * -------------------------------------------
 *   - When a conn's buffer is empty, we only listen for EPOLLIN.
 *   - When EPOLLIN fires, we read up to CONN_BUF_SIZE bytes into the
 *     buffer and immediately switch interest to EPOLLOUT.
 *   - When EPOLLOUT fires, we write what's left from the buffer; once
 *     drained we switch interest back to EPOLLIN.
 *   - That toggling is what saves us from needing a queue: at any
 *     moment a connection is either "wants to read" or "wants to
 *     write", never both. It also throttles each client to 4 KiB
 *     in-flight, which is fine for an echo demo.
 *
 * Allocation summary
 * ------------------
 *   site                          allocates              freed by
 *   ---------------------------   --------------------   --------------------
 *   accept_clients (new client)   one conn_t             close_conn (EOF /
 *                                                        error / EPOLLHUP) or
 *                                                        teardown loop in
 *                                                        net_run on shutdown
 *
 *   The conn_t's `buf[]` is an inline array (no separate malloc), and the
 *   hashtable_t pointer is borrowed. So there is exactly one malloc/free
 *   pairing in this whole file.
 */

#define _GNU_SOURCE  /* for accept4(), if available -- harmless otherwise */

#include "networking.h"

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
#define CONN_BUF_SIZE 4096         /* per-connection echo buffer            */

/* ------------------------------------------------------------------------- */
/* Per-connection state                                                      */
/* ------------------------------------------------------------------------- */

typedef struct conn {
    int           fd;
    unsigned char buf[CONN_BUF_SIZE];
    size_t        buf_len;     /* total bytes currently held in buf     */
    size_t        buf_off;     /* bytes already written from buf        */
    uint32_t      events;      /* current epoll interest mask           */
    struct conn  *next;        /* link in g_conns list                  */
} conn_t;

/* ------------------------------------------------------------------------- */
/* Module-private state                                                      */
/* ------------------------------------------------------------------------- */

static hashtable_t            *g_store     = NULL;   /* borrowed; step 2     */
static int                     g_listen_fd = -1;
static int                     g_epoll_fd  = -1;
static conn_t                 *g_conns     = NULL;   /* head of conn list    */
static volatile sig_atomic_t   g_shutdown  = 0;      /* set from signal hdlr */

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
    /* Read the current flags, OR in O_NONBLOCK, write them back. We
     * don't clobber unrelated flags this way. */
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

    /* SO_REUSEADDR lets us rebind to the port immediately after a
     * crash/exit instead of waiting for TIME_WAIT to drain. */
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
    /* No-op fast path: avoid a syscall when the interest mask isn't
     * actually changing. EPOLL_CTL_MOD is cheap but not free. */
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
    /* In level-triggered mode we technically only need to accept once
     * per event -- the kernel will re-notify if more are pending. But
     * draining is cheap and reduces wakeups under load, so we loop
     * until accept() returns EAGAIN/EWOULDBLOCK. */
    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof peer;
        int                cfd      = accept(g_listen_fd,
                                             (struct sockaddr *)&peer,
                                             &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  /* no more pending connections */
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
        c->fd      = cfd;
        c->buf_len = 0;
        c->buf_off = 0;
        c->events  = 0;
        c->next    = g_conns;
        g_conns    = c;

        /* Register with epoll. Start in "wants to read" mode since the
         * buffer is empty. arm_events() uses EPOLL_CTL_MOD, so we do
         * the initial ADD by hand here. */
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events   = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            perror("epoll_ctl(ADD)");
            /* Unlink and free -- conn_t was just prepended to g_conns. */
            g_conns = c->next;
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
    /* EPOLL_CTL_DEL before close() to be tidy; the kernel removes the
     * fd from the epoll set on close anyway, but being explicit keeps
     * us safe against dup()/fork() surprises later. */
    if (g_epoll_fd >= 0) {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    }
    close(c->fd);

    /* Unlink from g_conns. The list is singly-linked and short under
     * the expected workload, so a linear scan is fine. */
    conn_t **link = &g_conns;
    while (*link != NULL && *link != c) {
        link = &(*link)->next;
    }
    if (*link == c) {
        *link = c->next;
    }

    fprintf(stderr, "[networking] closed fd=%d\n", c->fd);
    free(c);   /* matches the malloc in accept_clients() */
}

/* ------------------------------------------------------------------------- */
/* I/O handlers                                                              */
/* ------------------------------------------------------------------------- */

static int handle_read(conn_t *c)
{
    /* Half-duplex: only read when the buffer is empty. If for some
     * reason EPOLLIN fires while we still have data to write, just
     * ignore it -- arm_events() will switch us off EPOLLIN shortly. */
    if (c->buf_len > 0) {
        return 0;
    }

    ssize_t n = read(c->fd, c->buf, sizeof c->buf);
    if (n == 0) {
        return -1;   /* peer closed -- caller will close_conn() */
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;  /* spurious wake-up; try again later */
        }
        perror("read");
        return -1;
    }

    c->buf_len = (size_t)n;
    c->buf_off = 0;

    /* Switch to write mode: we now have bytes to ship back. */
    return arm_events(c, EPOLLOUT);
}

static int handle_write(conn_t *c)
{
    if (c->buf_len == 0) {
        /* Shouldn't happen with the toggle scheme, but be defensive
         * and just go back to reading. */
        return arm_events(c, EPOLLIN);
    }

    size_t  pending = c->buf_len - c->buf_off;
    ssize_t n       = write(c->fd, c->buf + c->buf_off, pending);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;  /* kernel buffer full; try again on next EPOLLOUT */
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return -1; /* peer hung up mid-write -- close cleanly */
        }
        perror("write");
        return -1;
    }

    c->buf_off += (size_t)n;
    if (c->buf_off >= c->buf_len) {
        /* Drained: clear the buffer and go back to reading. */
        c->buf_len = 0;
        c->buf_off = 0;
        return arm_events(c, EPOLLIN);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Signal handling                                                           */
/* ------------------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    /* Setting a sig_atomic_t is the only thing we're allowed to do
     * safely in a signal handler with these signatures. epoll_wait()
     * will return EINTR and the main loop checks g_shutdown. */
    g_shutdown = 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int net_init(hashtable_t *store)
{
    g_store = store;  /* borrowed; step 2 will dispatch commands against it */

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
     * for "this event is on the listener" inside the dispatch switch. */
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

    /* Install signal handlers for graceful shutdown. SA_RESTART is
     * deliberately *not* set on epoll_wait -- we want EINTR so the
     * loop checks g_shutdown promptly. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGPIPE would kill us if a peer closes mid-write. We handle the
     * EPIPE return from write() ourselves, so just ignore the signal. */
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
                /* Likely SIGINT -- re-check the shutdown flag and either
                 * exit cleanly or keep going. */
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

    /* Teardown: free every live conn, then the epoll/listen fds. We do
     * this even on error paths so valgrind reports a clean exit. */
    fprintf(stderr, "[networking] shutting down...\n");
    while (g_conns != NULL) {
        conn_t *next = g_conns->next;
        if (g_epoll_fd >= 0) {
            epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, g_conns->fd, NULL);
        }
        close(g_conns->fd);
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
