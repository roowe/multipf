#pragma once

#include <sys/types.h>

namespace gopf {

struct Event {
    void* ptr = nullptr;
    bool readable = false;
    bool writable = false;
    bool error = false;
};

/* Readiness abstraction: kqueue on macOS, epoll on Linux.
 * A single loop instance drives every connection. */
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add(int fd, bool want_read, bool want_write, void* ptr);
    void modify(int fd, bool want_read, bool want_write, void* ptr);
    void remove(int fd);

    /* Returns ready event count, 0 on timeout, -1 on error. */
    int wait(Event* out, int max_events, int timeout_ms);

private:
    int fd_ = -1;
};

} // namespace gopf
