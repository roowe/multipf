#include "ev.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

namespace gopf {

EventLoop::EventLoop()
{
#ifdef __linux__
    fd_ = epoll_create1(EPOLL_CLOEXEC);
#else
    fd_ = kqueue();
#endif
    if (fd_ < 0)
        throw std::system_error(errno, std::generic_category(), "event loop");
}

EventLoop::~EventLoop()
{
    if (fd_ >= 0)
        close(fd_);
}

#ifndef __linux__

namespace {

void apply_kqueue(int kq, int fd, int16_t filter, bool want, void* ptr)
{
    struct kevent kev;
    unsigned short flags = want ? (EV_ADD | EV_ENABLE) : EV_DELETE;
    EV_SET(&kev, fd, filter, flags, 0, 0, ptr);

    if (kevent(kq, &kev, 1, nullptr, 0, nullptr) < 0) {
        /* Removing a filter that was never installed is fine. */
        if (!want && (errno == ENOENT || errno == EBADF))
            return;
        throw std::system_error(errno, std::generic_category(), "kevent");
    }
}

} // namespace

void EventLoop::add(int fd, bool want_read, bool want_write, void* ptr)
{
    modify(fd, want_read, want_write, ptr);
}

void EventLoop::modify(int fd, bool want_read, bool want_write, void* ptr)
{
    apply_kqueue(fd_, fd, EVFILT_READ, want_read, ptr);
    apply_kqueue(fd_, fd, EVFILT_WRITE, want_write, ptr);
}

void EventLoop::remove(int fd)
{
    try {
        apply_kqueue(fd_, fd, EVFILT_READ, false, nullptr);
    } catch (const std::exception&) {
    }
    try {
        apply_kqueue(fd_, fd, EVFILT_WRITE, false, nullptr);
    } catch (const std::exception&) {
    }
}

int EventLoop::wait(Event* out, int max_events, int timeout_ms)
{
    std::vector<struct kevent> events(static_cast<size_t>(max_events));
    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = kevent(fd_, nullptr, 0, events.data(), max_events, tsp);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    for (int i = 0; i < n; ++i) {
        Event& ev = out[i];
        ev = Event{};
        ev.ptr = events[i].udata;

        if (events[i].flags & EV_ERROR) {
            ev.error = true;
            continue;
        }
        if (events[i].filter == EVFILT_READ)
            ev.readable = true; /* EV_EOF lands here too: recv() returns 0 */
        else if (events[i].filter == EVFILT_WRITE)
            ev.writable = true;
    }
    return n;
}

#else /* __linux__ */

void EventLoop::add(int fd, bool want_read, bool want_write, void* ptr)
{
    struct epoll_event ev{};
    ev.events = EPOLLRDHUP;
    if (want_read)
        ev.events |= EPOLLIN;
    if (want_write)
        ev.events |= EPOLLOUT;
    ev.data.ptr = ptr;

    if (epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        if (errno == EEXIST) {
            if (epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
            return;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

void EventLoop::modify(int fd, bool want_read, bool want_write, void* ptr)
{
    if (!want_read && !want_write) {
        remove(fd);
        return;
    }
    add(fd, want_read, want_write, ptr);
}

void EventLoop::remove(int fd)
{
    if (epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        if (errno != ENOENT && errno != EBADF)
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

int EventLoop::wait(Event* out, int max_events, int timeout_ms)
{
    std::vector<struct epoll_event> events(static_cast<size_t>(max_events));
    int n = epoll_wait(fd_, events.data(), max_events, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }

    for (int i = 0; i < n; ++i) {
        Event& ev = out[i];
        ev = Event{};
        ev.ptr = events[i].data.ptr;
        ev.readable = (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;
        ev.writable = (events[i].events & EPOLLOUT) != 0;
        ev.error = (events[i].events & EPOLLERR) != 0;
    }
    return n;
}

#endif

} // namespace gopf
