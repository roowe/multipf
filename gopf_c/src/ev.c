#define _GNU_SOURCE
#include "ev.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

struct EvLoop {
#ifdef __linux__
    int epfd;
#else
    int kq;
#endif
};

EvLoop *ev_loop_new(void)
{
#ifdef __linux__
    EvLoop *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0) {
        free(l);
        return NULL;
    }
    return l;
#else
    EvLoop *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->kq = kqueue();
    if (l->kq < 0) {
        free(l);
        return NULL;
    }
    return l;
#endif
}

void ev_loop_free(EvLoop *loop)
{
    if (!loop)
        return;
#ifdef __linux__
    if (loop->epfd >= 0)
        close(loop->epfd);
#else
    if (loop->kq >= 0)
        close(loop->kq);
#endif
    free(loop);
}

/* ------------------------------------------------------------------ */
/* kqueue implementation                                               */
/* ------------------------------------------------------------------ */

#ifndef __linux__

static int kq_apply(EvLoop *l, int fd, int16_t filter, int want, void *ptr)
{
    struct kevent kev;
    unsigned short flags = want ? (EV_ADD | EV_ENABLE) : EV_DELETE;
    EV_SET(&kev, fd, filter, flags, 0, 0, ptr);

    if (kevent(l->kq, &kev, 1, NULL, 0, NULL) < 0) {
        /* Deleting a filter that was never added is not an error. */
        if (!want && (errno == ENOENT || errno == EBADF))
            return 0;
        return -1;
    }
    return 0;
}

int ev_add(EvLoop *loop, int fd, int want_read, int want_write, void *ptr)
{
    return ev_mod(loop, fd, want_read, want_write, ptr);
}

int ev_mod(EvLoop *loop, int fd, int want_read, int want_write, void *ptr)
{
    /* Pushing both filters keeps the call idempotent: adding an
     * already-registered filter simply updates its udata. */
    if (kq_apply(loop, fd, EVFILT_READ, want_read, ptr) < 0)
        return -1;
    if (kq_apply(loop, fd, EVFILT_WRITE, want_write, ptr) < 0)
        return -1;
    return 0;
}

int ev_del(EvLoop *loop, int fd)
{
    if (kq_apply(loop, fd, EVFILT_READ, 0, NULL) < 0)
        return -1;
    return kq_apply(loop, fd, EVFILT_WRITE, 0, NULL);
}

int ev_wait(EvLoop *loop, EvEvent *out, int maxevents, int timeout_ms)
{
    struct kevent events[64];
    int cap = maxevents < (int)(sizeof(events) / sizeof(events[0]))
                  ? maxevents
                  : (int)(sizeof(events) / sizeof(events[0]));

    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(loop->kq, NULL, 0, events, cap, tsp);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        struct kevent *kev = &events[i];
        EvEvent *e = &out[i];
        e->ptr = kev->udata;
        e->readable = 0;
        e->writable = 0;
        e->err = 0;

        if (kev->flags & EV_ERROR) {
            e->err = 1;
            continue;
        }
        if (kev->filter == EVFILT_READ) {
            e->readable = 1; /* EV_EOF also lands here: recv() returns 0 */
        } else if (kev->filter == EVFILT_WRITE) {
            e->writable = 1;
        }
    }
    return n;
}

#else /* __linux__ */

int ev_add(EvLoop *loop, int fd, int want_read, int want_write, void *ptr)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLRDHUP;
    if (want_read)
        ev.events |= EPOLLIN;
    if (want_write)
        ev.events |= EPOLLOUT;
    ev.data.ptr = ptr;

    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EEXIST)
            return epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev);
        return -1;
    }
    return 0;
}

int ev_mod(EvLoop *loop, int fd, int want_read, int want_write, void *ptr)
{
    if (!want_read && !want_write)
        return ev_del(loop, fd);
    return ev_add(loop, fd, want_read, want_write, ptr);
}

int ev_del(EvLoop *loop, int fd)
{
    if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) < 0)
        return (errno == ENOENT || errno == EBADF) ? 0 : -1;
    return 0;
}

int ev_wait(EvLoop *loop, EvEvent *out, int maxevents, int timeout_ms)
{
    struct epoll_event events[64];
    int cap = maxevents < (int)(sizeof(events) / sizeof(events[0]))
                  ? maxevents
                  : (int)(sizeof(events) / sizeof(events[0]));

    int n = epoll_wait(loop->epfd, events, cap, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        EvEvent *e = &out[i];
        uint32_t mask = events[i].events;
        e->ptr = events[i].data.ptr;
        e->readable = (mask & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
        e->writable = (mask & EPOLLOUT) != 0;
        e->err = (mask & EPOLLERR) != 0;
    }
    return n;
}

#endif /* __linux__ */
