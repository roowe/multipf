#ifndef GPROXY_H
#define GPROXY_H

#include "backend.h"

/* Wake codes written into the self-pipe by signal handlers. */
#define WAKE_RELOAD 1
#define WAKE_STATUS 2
#define WAKE_STOP   3

/* Runs the accept/forward loop until a stop signal arrives.
 * Returns 0 on clean shutdown, -1 on fatal error. */
int proxy_run(int port, BackendStore *store, const char *config_path, int wake_fd);

#endif /* GPROXY_H */
