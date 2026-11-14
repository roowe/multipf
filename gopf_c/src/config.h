#ifndef GCONF_H
#define GCONF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char host[128];
    int port;
    int weight;
} Host;

typedef struct {
    Host *hosts;
    size_t nhosts;
    int total_weight;
} Backend;

/* Parse a gopf JSON config file. Returns 0 on success, -1 on failure
 * (with a human-readable message written into err). */
int config_load(const char *path, Backend *out, char *err, size_t errlen);

void backend_free(Backend *backend);

#endif /* GCONF_H */
