/*
 * networking.c - Phase 2 epoll loop placeholder.
 *
 * Nothing here allocates or frees yet, so there is no memory contract
 * to document. When the real epoll loop lands, every per-connection
 * malloc will be paired with the matching free at EPOLLHUP / close()
 * time, and the documentation will move here alongside it.
 */

#include "networking.h"

#include <stdio.h>

/* Module-private handle to the shared store. The event loop will reach
 * for this on every command dispatch; keeping it file-static avoids
 * threading the pointer through every callback in Phase 2. */
static hashtable_t *g_store = NULL;

int net_init(hashtable_t *store)
{
    g_store = store;
    fprintf(stderr, "[networking] init: store=%p (epoll loop placeholder - phase 2)\n",
            (void *)store);
    return 0;
}

int net_run(void)
{
    fprintf(stderr, "[networking] run: epoll loop placeholder - phase 2 (returning immediately)\n");
    /* Touch g_store so -Wunused-but-set-variable stays quiet under
     * stricter warning levels even before the real loop exists. */
    (void)g_store;
    return 0;
}
