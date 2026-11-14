#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "proxy.h"

static int g_wake_fd = -1;

static void on_signal(int signo)
{
    char code;
    switch (signo) {
    case SIGHUP:
        code = WAKE_RELOAD;
        break;
    case SIGUSR1:
        code = WAKE_STATUS;
        break;
    case SIGINT:
    case SIGTERM:
        code = WAKE_STOP;
        break;
    default:
        return;
    }
    if (g_wake_fd >= 0)
        write(g_wake_fd, &code, 1);
}

static int make_wake_pipe(int *read_fd, int *write_fd)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags >= 0)
            fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    *read_fd = fds[0];
    *write_fd = fds[1];
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--listen <port>] <config.json>\n"
            "  SIGHUP  reload config\n"
            "  SIGUSR1 print connection status\n", prog);
}

int main(int argc, char **argv)
{
    int port = 1254;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 || strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            port = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--listen=", 9) == 0) {
            port = atoi(argv[i] + 9);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            config_path = argv[i];
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %d\n", port);
        return 2;
    }
    if (!config_path) {
        usage(argv[0]);
        return 2;
    }

    BackendStore store;
    char err[256] = {0};
    if (backend_store_init(&store, config_path, err, sizeof(err)) < 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    /* Shells and supervisors sometimes start children with signals masked;
     * the mask is inherited across exec, which would silently disable
     * reload/status/shutdown. Clear the ones we rely on. */
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGHUP);
    sigaddset(&unblock, SIGINT);
    sigaddset(&unblock, SIGTERM);
    sigaddset(&unblock, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);

    int wake_read = -1, wake_write = -1;
    if (make_wake_pipe(&wake_read, &wake_write) < 0) {
        fprintf(stderr, "pipe: %s\n", strerror(errno));
        backend_store_free(&store);
        return 1;
    }
    g_wake_fd = wake_write;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    int rc = proxy_run(port, &store, config_path, wake_read);

    g_wake_fd = -1;
    close(wake_read);
    close(wake_write);
    backend_store_free(&store);
    return rc == 0 ? 0 : 1;
}
