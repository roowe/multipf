/*
 * ev.h — thin cross-platform readiness event loop.
 *
 * macOS  -> kqueue
 * Linux  -> epoll
 *
 * A single EvLoop drives all connections. Each file descriptor is
 * registered with an opaque pointer (typically a per-connection
 * channel descriptor) so the caller can map readiness back to a
 * connection direction.
 */
#ifndef GEV_H
#define GEV_H

typedef struct {
    void *ptr;      /* opaque pointer supplied at registration time */
    int readable;   /* read readiness (or EOF / hangup) */
    int writable;   /* write readiness */
    int err;        /* hard error reported by the kernel */
} EvEvent;

typedef struct EvLoop EvLoop;

EvLoop *ev_loop_new(void);
void ev_loop_free(EvLoop *loop);

/* Add or update interest. want_read / want_write are booleans. */
int ev_add(EvLoop *loop, int fd, int want_read, int want_write, void *ptr);
int ev_mod(EvLoop *loop, int fd, int want_read, int want_write, void *ptr);
int ev_del(EvLoop *loop, int fd);

/* Returns the number of ready events, 0 on timeout, -1 on error. */
int ev_wait(EvLoop *loop, EvEvent *out, int maxevents, int timeout_ms);

#endif /* GEV_H */
