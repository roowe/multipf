#define _GNU_SOURCE
#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ev.h"

#define IO_CHUNK 16384
#define LISTEN_BACKLOG 4096
#define CONNECT_TIMEOUT_MS 5000
#define KEEPALIVE_IDLE_S 60
#define MAX_EVENTS 64

#ifdef __APPLE__
#define SEND_FLAGS 0
#else
#define SEND_FLAGS MSG_NOSIGNAL
#endif

enum { DIR_C2U = 0, DIR_U2C = 1 };
enum { ST_CONNECTING = 1, ST_FORWARDING = 2 };

typedef struct Conn Conn;

/* One socket of a proxied connection. The address of this struct is the
 * opaque pointer handed to the event loop, which lets kqueue (two filters
 * per fd) and epoll (one entry per fd) share the same dispatch path. */
typedef struct {
    Conn *conn;
    int is_up;
    int fd;
    int reg_r; /* interest currently registered with the kernel */
    int reg_w;
} Chan;

struct Conn {
    Conn *next;

    int cli_fd;
    int up_fd;

    int state;
    int cli_eof; /* client has finished sending */
    int up_eof;  /* upstream has finished sending */
    int closing;

    /* Bytes read from one side but not yet written to the other.
     * Data is only read when this buffer is empty, which gives us
     * natural backpressure without unbounded buffering. */
    uint8_t c2u_buf[IO_CHUNK];
    size_t c2u_len;
    uint8_t u2c_buf[IO_CHUNK];
    size_t u2c_len;

    uint64_t created_ms;

    Chan ch_cli;
    Chan ch_up;
};

typedef struct {
    EvLoop *loop;
    BackendStore *store;
    const char *config_path;
    int listen_fd;
    int wake_fd;

    Conn *conns;
    Conn *graveyard; /* closed this iteration, freed after dispatch */
    size_t nconns;
    uint64_t rng;
} Proxy;

static char tag_listen;
static char tag_wake;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_socket_options(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    int idle = KEEPALIVE_IDLE_S;
#ifdef __APPLE__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
}

/* ------------------------------------------------------------------ */
/* Connection lifecycle                                                */
/* ------------------------------------------------------------------ */

static void conn_close(Proxy *p, Conn *c)
{
    if (c->closing)
        return;
    c->closing = 1;

    if (c->cli_fd >= 0) {
        ev_del(p->loop, c->cli_fd);
        close(c->cli_fd);
        c->cli_fd = -1;
    }
    if (c->up_fd >= 0) {
        ev_del(p->loop, c->up_fd);
        close(c->up_fd);
        c->up_fd = -1;
    }

    Conn **pp = &p->conns;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
        pp = &(*pp)->next;
    }
    p->nconns--;

    /* A batch can contain several events for the same connection (kqueue
     * reports read and write filters separately). Defer the free until
     * the batch has been dispatched so a stale event sees closing=1
     * instead of freed memory. */
    c->next = p->graveyard;
    p->graveyard = c;
}

static void reap(Proxy *p)
{
    Conn *c = p->graveyard;
    while (c) {
        Conn *next = c->next;
        free(c);
        c = next;
    }
    p->graveyard = NULL;
}

static void maybe_close(Proxy *p, Conn *c)
{
    if (c->cli_eof && c->up_eof && c->c2u_len == 0 && c->u2c_len == 0)
        conn_close(p, c);
}

static size_t *dir_buf_len(Conn *c, int dir)
{
    return dir == DIR_C2U ? &c->c2u_len : &c->u2c_len;
}

static uint8_t *dir_buf(Conn *c, int dir)
{
    return dir == DIR_C2U ? c->c2u_buf : c->u2c_buf;
}

static void update_interest(Proxy *p, Conn *c);

/* Only touch the kernel when the desired interest actually changes.
 * Without this cache every read/write transition would issue redundant
 * kevent/epoll syscalls. */
static int set_interest(Proxy *p, Chan *chan, int want_read, int want_write)
{
    if (chan->reg_r == want_read && chan->reg_w == want_write)
        return 0;
    if (ev_mod(p->loop, chan->fd, want_read, want_write, chan) == 0) {
        chan->reg_r = want_read;
        chan->reg_w = want_write;
        return 0;
    }
    return -1;
}

/* Write out everything pending in one direction. */
static void dir_flush(Proxy *p, Conn *c, int dir)
{
    uint8_t *buf = dir_buf(c, dir);
    size_t *len = dir_buf_len(c, dir);
    int dst = dir == DIR_C2U ? c->up_fd : c->cli_fd;

    while (*len > 0) {
        ssize_t sent = send(dst, buf, *len, SEND_FLAGS);
        if (sent > 0) {
            if ((size_t)sent == *len) {
                *len = 0;
                break;
            }
            memmove(buf, buf + sent, *len - (size_t)sent);
            *len -= (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (sent < 0 && errno == EINTR)
            continue;
        conn_close(p, c);
        return;
    }

    update_interest(p, c);
}

/* Read one chunk from the source and push it towards the destination. */
static void dir_read(Proxy *p, Conn *c, int dir)
{
    int src = dir == DIR_C2U ? c->cli_fd : c->up_fd;
    int dst = dir == DIR_C2U ? c->up_fd : c->cli_fd;
    uint8_t *buf = dir_buf(c, dir);
    size_t *len = dir_buf_len(c, dir);

    if (*len > 0)
        return; /* pending write: do not read more */

    ssize_t n = recv(src, buf, IO_CHUNK, 0);
    if (n == 0) {
        /* Source finished sending: propagate the half-close. */
        if (dir == DIR_C2U)
            c->cli_eof = 1;
        else
            c->up_eof = 1;

        shutdown(dst, SHUT_WR);
        maybe_close(p, c);
        update_interest(p, c);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            update_interest(p, c);
            return;
        }
        if (errno == EINTR)
            return;
        conn_close(p, c);
        return;
    }

    *len = (size_t)n;
    dir_flush(p, c, dir);
}

static void update_interest(Proxy *p, Conn *c)
{
    if (c->closing)
        return;

    if (c->state == ST_CONNECTING) {
        set_interest(p, &c->ch_up, 0, 1);
        return;
    }

    /* Read only when the corresponding write buffer is empty; write
     * only when there is something to write. */
    set_interest(p, &c->ch_cli,
                 !c->cli_eof && c->c2u_len == 0,
                 c->u2c_len > 0);
    set_interest(p, &c->ch_up,
                 !c->up_eof && c->u2c_len == 0,
                 c->c2u_len > 0);
}

/* ------------------------------------------------------------------ */
/* Upstream connection                                                 */
/* ------------------------------------------------------------------ */

static Conn *conn_begin(Proxy *p, int cli_fd, const Host *host)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char portstr[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%d", host->port);
    if (getaddrinfo(host->host, portstr, &hints, &res) != 0)
        return NULL;

    int up_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (up_fd < 0) {
        freeaddrinfo(res);
        return NULL;
    }

    if (set_nonblock(up_fd) < 0) {
        freeaddrinfo(res);
        close(up_fd);
        return NULL;
    }
    set_socket_options(up_fd);

    Conn *c = calloc(1, sizeof(*c));
    if (!c) {
        freeaddrinfo(res);
        close(up_fd);
        return NULL;
    }

    c->cli_fd = cli_fd;
    c->up_fd = up_fd;
    c->state = ST_CONNECTING;
    c->created_ms = now_ms();
    c->ch_cli.conn = c;
    c->ch_cli.is_up = 0;
    c->ch_cli.fd = cli_fd;
    c->ch_up.conn = c;
    c->ch_up.is_up = 1;
    c->ch_up.fd = up_fd;

    int rc = connect(up_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc == 0) {
        c->state = ST_FORWARDING;
    } else if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(up_fd);
        free(c);
        return NULL;
    }

    c->next = p->conns;
    p->conns = c;
    p->nconns++;

    if (set_interest(p, &c->ch_up, 0, 1) < 0) {
        /* The caller still owns cli_fd; detach it before cleanup. */
        c->cli_fd = -1;
        conn_close(p, c);
        return NULL;
    }

    update_interest(p, c);
    return c;
}

/* ------------------------------------------------------------------ */
/* Event dispatch                                                      */
/* ------------------------------------------------------------------ */

static void on_connect_ready(Proxy *p, Conn *c)
{
    int so_error = 0;
    socklen_t len = sizeof(so_error);

    if (getsockopt(c->up_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        conn_close(p, c);
        return;
    }
    if (so_error != 0) {
        conn_close(p, c);
        return;
    }

    c->state = ST_FORWARDING;
    update_interest(p, c);
}

static void handle_chan_event(Proxy *p, EvEvent *e)
{
    Chan *chan = (Chan *)e->ptr;
    Conn *c = chan->conn;

    if (c->closing)
        return;

    if (!chan->is_up) {
        if (e->err) {
            conn_close(p, c);
            return;
        }
        if (e->writable)
            dir_flush(p, c, DIR_U2C);
        if (!c->closing && e->readable)
            dir_read(p, c, DIR_C2U);
        return;
    }

    if (c->state == ST_CONNECTING) {
        if (e->err || e->writable || e->readable)
            on_connect_ready(p, c);
        return;
    }

    if (e->err) {
        conn_close(p, c);
        return;
    }
    if (e->writable)
        dir_flush(p, c, DIR_C2U);
    if (!c->closing && e->readable)
        dir_read(p, c, DIR_U2C);
}

static void do_accept(Proxy *p)
{
    for (;;) {
        int fd = accept(p->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }

        if (set_nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        set_socket_options(fd);

        const Host *host = backend_choose(backend_store_get(p->store), &p->rng);
        if (!host) {
            close(fd);
            continue;
        }

        if (!conn_begin(p, fd, host)) {
            /* conn_begin only leaves the client fd open on failure
             * before taking ownership. */
            if (fd >= 0)
                close(fd);
        }
    }
}

static void sweep_timeouts(Proxy *p)
{
    uint64_t now = now_ms();
    Conn *c = p->conns;
    while (c) {
        Conn *next = c->next;
        if (c->state == ST_CONNECTING && now - c->created_ms > CONNECT_TIMEOUT_MS)
            conn_close(p, c);
        c = next;
    }
}

static void handle_wake(Proxy *p, int *stop)
{
    char codes[16];
    ssize_t n;

    while ((n = read(p->wake_fd, codes, sizeof(codes))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (codes[i] == WAKE_RELOAD) {
                char err[256] = {0};
                if (backend_store_reload(p->store, p->config_path, err, sizeof(err)) == 0)
                    fprintf(stderr, "reloaded config: %zu hosts\n",
                            backend_store_get(p->store)->nhosts);
                else
                    fprintf(stderr, "reload failed: %s\n", err);
            } else if (codes[i] == WAKE_STATUS) {
                fprintf(stderr, "status: connections=%zu\n", p->nconns);
            } else if (codes[i] == WAKE_STOP) {
                *stop = 1;
            }
        }
    }
}

static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int proxy_run(int port, BackendStore *store, const char *config_path, int wake_fd)
{
    Proxy p;
    memset(&p, 0, sizeof(p));
    p.store = store;
    p.config_path = config_path;
    p.wake_fd = wake_fd;
    p.rng = (uint64_t)now_ms() ^ ((uint64_t)getpid() << 32);
    if (p.rng == 0)
        p.rng = 0x9e3779b97f4a7c15ULL;

    p.loop = ev_loop_new();
    if (!p.loop) {
        fprintf(stderr, "failed to create event loop: %s\n", strerror(errno));
        return -1;
    }

    p.listen_fd = make_listener(port);
    if (p.listen_fd < 0) {
        fprintf(stderr, "failed to listen on port %d: %s\n", port, strerror(errno));
        ev_loop_free(p.loop);
        return -1;
    }

    if (ev_add(p.loop, p.listen_fd, 1, 0, &tag_listen) < 0 ||
        ev_add(p.loop, p.wake_fd, 1, 0, &tag_wake) < 0) {
        fprintf(stderr, "failed to register fds: %s\n", strerror(errno));
        close(p.listen_fd);
        ev_loop_free(p.loop);
        return -1;
    }

    fprintf(stderr, "gopf (C) listening on 0.0.0.0:%d, %zu backend(s)\n",
            port, backend_store_get(store)->nhosts);

    int stop = 0;
    uint64_t last_sweep = now_ms();
    EvEvent events[MAX_EVENTS];

    while (!stop) {
        int n = ev_wait(p.loop, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "event loop error: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            EvEvent *e = &events[i];
            if (e->ptr == &tag_wake) {
                handle_wake(&p, &stop);
            } else if (e->ptr == &tag_listen) {
                do_accept(&p);
            } else if (e->ptr != NULL) {
                handle_chan_event(&p, e);
            }
        }

        uint64_t now = now_ms();
        if (now - last_sweep >= 1000) {
            sweep_timeouts(&p);
            last_sweep = now;
        }
        reap(&p);
    }

    /* Close every live connection on shutdown. */
    while (p.conns)
        conn_close(&p, p.conns);
    reap(&p);

    ev_del(p.loop, p.listen_fd);
    close(p.listen_fd);
    ev_loop_free(p.loop);
    return 0;
}
