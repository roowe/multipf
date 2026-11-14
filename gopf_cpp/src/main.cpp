#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "backend.hpp"
#include "proxy.hpp"

namespace {

int g_wake_fd = -1;

extern "C" void on_signal(int signo)
{
    char code = 0;
    switch (signo) {
    case SIGHUP:
        code = gopf::WAKE_RELOAD;
        break;
    case SIGUSR1:
        code = gopf::WAKE_STATUS;
        break;
    case SIGINT:
    case SIGTERM:
        code = gopf::WAKE_STOP;
        break;
    default:
        return;
    }
    if (g_wake_fd >= 0)
        write(g_wake_fd, &code, 1);
}

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "usage: %s [--listen <port>] <config.json>\n"
                 "  SIGHUP  reload config\n"
                 "  SIGUSR1 print connection status\n",
                 prog);
}

int make_wake_pipe(int& read_fd, int& write_fd)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    for (int fd : fds) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    read_fd = fds[0];
    write_fd = fds[1];
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    int port = 1255;
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 || std::strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            port = std::atoi(argv[++i]);
        } else if (std::strncmp(argv[i], "--listen=", 9) == 0) {
            port = std::atoi(argv[i] + 9);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            config_path = argv[i];
        }
    }

    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "invalid port: %d\n", port);
        return 2;
    }
    if (config_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    gopf::BackendStore store;
    try {
        store.init(config_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    /* Supervisors sometimes spawn children with signals masked; the mask
     * survives exec and would silently disable reload and shutdown. */
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGHUP);
    sigaddset(&unblock, SIGINT);
    sigaddset(&unblock, SIGTERM);
    sigaddset(&unblock, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &unblock, nullptr);

    int wake_read = -1;
    int wake_write = -1;
    if (make_wake_pipe(wake_read, wake_write) < 0) {
        std::fprintf(stderr, "pipe: %s\n", std::strerror(errno));
        return 1;
    }
    g_wake_fd = wake_write;

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);

    int rc = 1;
    try {
        gopf::Proxy proxy(port, store, config_path, wake_read);
        rc = proxy.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        rc = 1;
    }

    g_wake_fd = -1;
    close(wake_read);
    close(wake_write);
    return rc;
}
