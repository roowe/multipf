#include "proxy.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ev.hpp"

namespace gopf {
namespace {

constexpr size_t kIoChunk = 16384;
constexpr int kListenBacklog = 4096;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kKeepaliveIdleSec = 60;
constexpr int kMaxEvents = 64;

#ifdef __APPLE__
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::system_error(errno, std::generic_category(), "fcntl");
}

void set_socket_options(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    int idle = kKeepaliveIdleSec;
#ifdef __APPLE__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
}

} // namespace

struct Proxy::Impl {
    enum class State { Connecting, Forwarding };
    enum Dir { C2U, U2C };

    struct Connection;

    /* One socket of a proxied connection. Its address is the token the
     * event loop hands back, so kqueue's per-filter events and epoll's
     * per-fd events share one dispatch path. */
    struct Channel {
        Connection* conn = nullptr;
        bool is_up = false;
        int fd = -1;
        bool reg_r = false;
        bool reg_w = false;
    };

    struct Connection {
        Channel cli;
        Channel up;

        int cli_fd = -1;
        int up_fd = -1;

        State state = State::Connecting;
        bool cli_eof = false;
        bool up_eof = false;
        bool closing = false;

        /* Bytes read on one side and not yet written to the other. A
         * source is only read when this buffer is empty. */
        std::array<uint8_t, kIoChunk> c2u_buf{};
        std::array<uint8_t, kIoChunk> u2c_buf{};
        size_t c2u_len = 0;
        size_t u2c_len = 0;

        int64_t created_ms = 0;

        std::list<std::unique_ptr<Connection>>::iterator self;
    };

    Impl(int port, BackendStore& store, std::string config_path, int wake_fd)
        : port_(port)
        , store_(store)
        , config_path_(std::move(config_path))
        , wake_fd_(wake_fd)
        , rng_(std::random_device{}() ^ static_cast<uint64_t>(now_ms()))
    {
    }

    ~Impl()
    {
        while (!conns_.empty())
            close_conn(conns_.front().get());
        graveyard_.clear();
    }

    int run();

    void do_accept();
    void start_connection(int cli_fd, const Host& host);
    void close_conn(Connection* conn);
    void maybe_close(Connection* conn);

    void set_interest(Channel& chan, bool want_read, bool want_write);
    void update_interest(Connection* conn);
    void flush(Connection* conn, Dir dir);
    void read_dir(Connection* conn, Dir dir);
    void handle_event(const Event& ev);
    void sweep_timeouts();
    void handle_wake(bool& stop);

    int make_listener();

    static uint8_t* buffer(Connection* conn, Dir dir)
    {
        return dir == C2U ? conn->c2u_buf.data() : conn->u2c_buf.data();
    }
    static size_t& buffer_len(Connection* conn, Dir dir)
    {
        return dir == C2U ? conn->c2u_len : conn->u2c_len;
    }

    int port_;
    BackendStore& store_;
    std::string config_path_;
    int listen_fd_ = -1;
    int wake_fd_ = -1;

    EventLoop loop_;
    std::list<std::unique_ptr<Connection>> conns_;
    std::list<std::unique_ptr<Connection>> graveyard_;
    size_t nconns_ = 0;
    std::mt19937_64 rng_;

    static char listen_tag;
    static char wake_tag;
};

char Proxy::Impl::listen_tag = 0;
char Proxy::Impl::wake_tag = 0;

int Proxy::Impl::make_listener()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "socket");

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::system_error(errno, std::generic_category(), "bind");
    if (listen(fd, kListenBacklog) < 0)
        throw std::system_error(errno, std::generic_category(), "listen");

    try {
        set_nonblock(fd);
    } catch (...) {
        close(fd);
        throw;
    }
    return fd;
}

void Proxy::Impl::set_interest(Channel& chan, bool want_read, bool want_write)
{
    if (chan.reg_r == want_read && chan.reg_w == want_write)
        return;

    loop_.modify(chan.fd, want_read, want_write, &chan);
    chan.reg_r = want_read;
    chan.reg_w = want_write;
}

void Proxy::Impl::update_interest(Connection* conn)
{
    if (conn->closing)
        return;

    if (conn->state == State::Connecting) {
        set_interest(conn->up, false, true);
        return;
    }

    /* Read only when the matching write buffer is empty; write only
     * when there is something to write. */
    set_interest(conn->cli, !conn->cli_eof && conn->c2u_len == 0, conn->u2c_len > 0);
    set_interest(conn->up, !conn->up_eof && conn->u2c_len == 0, conn->c2u_len > 0);
}

void Proxy::Impl::close_conn(Connection* conn)
{
    if (conn->closing)
        return;
    conn->closing = true;

    if (conn->cli_fd >= 0) {
        try {
            loop_.remove(conn->cli_fd);
        } catch (const std::exception&) {
        }
        close(conn->cli_fd);
        conn->cli_fd = -1;
    }
    if (conn->up_fd >= 0) {
        try {
            loop_.remove(conn->up_fd);
        } catch (const std::exception&) {
        }
        close(conn->up_fd);
        conn->up_fd = -1;
    }

    /* A batch may hold several events for one connection (kqueue reports
     * read and write filters separately). Move the node to a graveyard
     * instead of destroying it, so a stale event sees closing=true
     * rather than freed memory. */
    conns_.splice(graveyard_.end(), conns_, conn->self);
    --nconns_;
}

void Proxy::Impl::maybe_close(Connection* conn)
{
    if (conn->cli_eof && conn->up_eof && conn->c2u_len == 0 && conn->u2c_len == 0)
        close_conn(conn);
}

void Proxy::Impl::flush(Connection* conn, Dir dir)
{
    uint8_t* buf = buffer(conn, dir);
    size_t& len = buffer_len(conn, dir);
    int dst = dir == C2U ? conn->up_fd : conn->cli_fd;

    while (len > 0) {
        ssize_t sent = send(dst, buf, len, kSendFlags);
        if (sent > 0) {
            if (static_cast<size_t>(sent) == len) {
                len = 0;
                break;
            }
            std::memmove(buf, buf + sent, len - static_cast<size_t>(sent));
            len -= static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (sent < 0 && errno == EINTR)
            continue;

        close_conn(conn);
        return;
    }

    update_interest(conn);
}

void Proxy::Impl::read_dir(Connection* conn, Dir dir)
{
    int src = dir == C2U ? conn->cli_fd : conn->up_fd;
    int dst = dir == C2U ? conn->up_fd : conn->cli_fd;
    uint8_t* buf = buffer(conn, dir);
    size_t& len = buffer_len(conn, dir);

    if (len > 0)
        return; /* pending write: apply backpressure */

    ssize_t n = recv(src, buf, kIoChunk, 0);
    if (n == 0) {
        /* Source finished sending: propagate the half-close. */
        if (dir == C2U)
            conn->cli_eof = true;
        else
            conn->up_eof = true;

        shutdown(dst, SHUT_WR);
        maybe_close(conn);
        update_interest(conn);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            update_interest(conn);
            return;
        }
        if (errno == EINTR)
            return;
        close_conn(conn);
        return;
    }

    len = static_cast<size_t>(n);
    flush(conn, dir);
}

void Proxy::Impl::start_connection(int cli_fd, const Host& host)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const std::string port = std::to_string(host.port);
    if (getaddrinfo(host.host.c_str(), port.c_str(), &hints, &res) != 0)
        throw std::runtime_error("resolve failed for " + host.host);

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(res, freeaddrinfo);

    int up_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (up_fd < 0)
        throw std::system_error(errno, std::generic_category(), "socket");

    bool inserted = false;
    Connection* raw = nullptr;

    try {
        set_nonblock(up_fd);
        set_socket_options(up_fd);

        auto conn = std::make_unique<Connection>();
        conn->cli_fd = cli_fd;
        conn->up_fd = up_fd;
        conn->created_ms = now_ms();

        conn->cli.conn = conn.get();
        conn->cli.is_up = false;
        conn->cli.fd = cli_fd;
        conn->up.conn = conn.get();
        conn->up.is_up = true;
        conn->up.fd = up_fd;

        int rc = connect(up_fd, res->ai_addr, res->ai_addrlen);
        if (rc == 0) {
            conn->state = State::Forwarding;
        } else if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            throw std::system_error(errno, std::generic_category(), "connect");
        }

        auto it = conns_.insert(conns_.end(), std::move(conn));
        raw = it->get();
        raw->self = it;
        ++nconns_;
        inserted = true;

        set_interest(raw->up, false, true);
        update_interest(raw);
    } catch (...) {
        if (inserted) {
            raw->cli_fd = -1; /* the caller still owns it */
            close_conn(raw);
        } else {
            close(up_fd);
        }
        throw;
    }
}

void Proxy::Impl::do_accept()
{
    for (;;) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        try {
            set_nonblock(fd);
            set_socket_options(fd);

            const Host* host = choose_host(store_.get(), rng_);
            if (!host) {
                close(fd);
                continue;
            }
            start_connection(fd, *host);
        } catch (const std::exception&) {
            close(fd);
        }
    }
}

void Proxy::Impl::sweep_timeouts()
{
    const int64_t now = now_ms();
    auto it = conns_.begin();
    while (it != conns_.end()) {
        Connection* conn = it->get();
        ++it;

        if (conn->state == State::Connecting && now - conn->created_ms > kConnectTimeoutMs)
            close_conn(conn);
    }
}

void Proxy::Impl::handle_event(const Event& ev)
{
    if (ev.ptr == &wake_tag) {
        bool stop = false;
        handle_wake(stop);
        if (stop)
            throw std::runtime_error("stop requested");
        return;
    }
    if (ev.ptr == &listen_tag) {
        do_accept();
        return;
    }
    if (!ev.ptr)
        return;

    Channel* chan = static_cast<Channel*>(ev.ptr);
    Connection* conn = chan->conn;
    if (conn->closing)
        return;

    if (!chan->is_up) {
        if (ev.error) {
            close_conn(conn);
            return;
        }
        if (ev.writable)
            flush(conn, U2C);
        if (!conn->closing && ev.readable)
            read_dir(conn, C2U);
        return;
    }

    if (conn->state == State::Connecting) {
        if (!ev.error && !ev.writable && !ev.readable)
            return;

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(conn->up_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            close_conn(conn);
            return;
        }
        conn->state = State::Forwarding;
        update_interest(conn);
        return;
    }

    if (ev.error) {
        close_conn(conn);
        return;
    }
    if (ev.writable)
        flush(conn, C2U);
    if (!conn->closing && ev.readable)
        read_dir(conn, U2C);
}

void Proxy::Impl::handle_wake(bool& stop)
{
    char codes[16];
    ssize_t n = 0;

    while ((n = read(wake_fd_, codes, sizeof(codes))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            if (codes[i] == WAKE_RELOAD) {
                try {
                    store_.reload(config_path_);
                    std::fprintf(stderr, "reloaded config: %zu hosts\n", store_.get().hosts.size());
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "reload failed: %s\n", e.what());
                }
            } else if (codes[i] == WAKE_STATUS) {
                std::fprintf(stderr, "status: connections=%zu\n", nconns_);
            } else if (codes[i] == WAKE_STOP) {
                stop = true;
            }
        }
    }
}

int Proxy::Impl::run()
{
    listen_fd_ = make_listener();
    loop_.add(listen_fd_, true, false, &listen_tag);
    loop_.add(wake_fd_, true, false, &wake_tag);

    std::fprintf(stderr, "gopf (C++) listening on 0.0.0.0:%d, %zu backend(s)\n",
                 port_, store_.get().hosts.size());

    bool stop = false;
    int64_t last_sweep = now_ms();
    std::vector<Event> events(kMaxEvents);

    while (!stop) {
        int n = loop_.wait(events.data(), kMaxEvents, 1000);
        if (n < 0)
            throw std::system_error(errno, std::generic_category(), "event wait");

        for (int i = 0; i < n && !stop; ++i) {
            try {
                handle_event(events[i]);
            } catch (const std::exception& e) {
                if (std::strcmp(e.what(), "stop requested") == 0)
                    stop = true;
                else
                    std::fprintf(stderr, "connection error: %s\n", e.what());
            }
        }

        const int64_t now = now_ms();
        if (now - last_sweep >= 1000) {
            sweep_timeouts();
            last_sweep = now;
        }
        graveyard_.clear();
    }

    while (!conns_.empty())
        close_conn(conns_.front().get());
    graveyard_.clear();

    try {
        loop_.remove(listen_fd_);
    } catch (const std::exception&) {
    }
    close(listen_fd_);
    listen_fd_ = -1;
    return 0;
}

Proxy::Proxy(int port, BackendStore& store, std::string config_path, int wake_fd)
    : impl_(std::make_unique<Impl>(port, store, std::move(config_path), wake_fd))
{
}

Proxy::~Proxy() = default;

int Proxy::run()
{
    return impl_->run();
}

} // namespace gopf
