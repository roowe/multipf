#include "backend.h"

#include <stdio.h>
#include <string.h>

int backend_store_init(BackendStore *store, const char *path, char *err, size_t errlen)
{
    memset(store, 0, sizeof(*store));
    return config_load(path, &store->backend, err, errlen);
}

int backend_store_reload(BackendStore *store, const char *path, char *err, size_t errlen)
{
    Backend fresh;
    if (config_load(path, &fresh, err, errlen) < 0)
        return -1;

    /* Swap only after a successful parse, so a broken config never
     * takes the proxy offline. */
    backend_free(&store->backend);
    store->backend = fresh;
    return 0;
}

const Backend *backend_store_get(BackendStore *store)
{
    return &store->backend;
}

void backend_store_free(BackendStore *store)
{
    backend_free(&store->backend);
}

const Host *backend_choose(const Backend *backend, uint64_t *rng_state)
{
    if (!backend || backend->nhosts == 0 || backend->total_weight <= 0)
        return NULL;

    uint64_t x = *rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *rng_state = x;

    int v = (int)(x % (uint64_t)backend->total_weight);
    for (size_t i = 0; i < backend->nhosts; i++) {
        if (v < backend->hosts[i].weight)
            return &backend->hosts[i];
        v -= backend->hosts[i].weight;
    }
    return NULL;
}
