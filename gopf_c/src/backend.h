#ifndef GBACKEND_H
#define GBACKEND_H

#include <stdint.h>

#include "config.h"

/* The store owns the active Backend. The proxy runs on a single thread,
 * so reloads can simply swap the structure without locking. */
typedef struct {
    Backend backend;
} BackendStore;

int backend_store_init(BackendStore *store, const char *path, char *err, size_t errlen);
int backend_store_reload(BackendStore *store, const char *path, char *err, size_t errlen);
const Backend *backend_store_get(BackendStore *store);
void backend_store_free(BackendStore *store);

/* Weighted random selection using a caller-owned xorshift64 state. */
const Host *backend_choose(const Backend *backend, uint64_t *rng_state);

#endif /* GBACKEND_H */
