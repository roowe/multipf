#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/cJSON.h"

/* "host:port" — the host may be empty (":8080"), in which case we
 * fall back to loopback, matching the Go reference behaviour. */
static int parse_addr(const char *addr, Host *host)
{
    const char *colon = strrchr(addr, ':');
    if (!colon)
        return -1;

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535)
        return -1;

    size_t hostlen = (size_t)(colon - addr);
    if (hostlen == 0) {
        snprintf(host->host, sizeof(host->host), "127.0.0.1");
    } else {
        if (hostlen >= sizeof(host->host))
            return -1;
        memcpy(host->host, addr, hostlen);
        host->host[hostlen] = '\0';
    }
    host->port = port;
    return 0;
}

int config_load(const char *path, Backend *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }

    long size = 0;
    if (fseek(fp, 0, SEEK_END) == 0)
        size = ftell(fp);
    if (size < 0)
        size = 0;
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(err, errlen, "invalid JSON in %s", path);
        return -1;
    }

    cJSON *hosts = cJSON_GetObjectItemCaseSensitive(root, "Hosts");
    if (!cJSON_IsArray(hosts)) {
        snprintf(err, errlen, "missing Hosts array");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(hosts);
    out->hosts = calloc((size_t)(count > 0 ? count : 1), sizeof(Host));
    if (!out->hosts) {
        snprintf(err, errlen, "out of memory");
        cJSON_Delete(root);
        return -1;
    }

    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, hosts) {
        cJSON *jaddr = cJSON_GetObjectItemCaseSensitive(item, "Addr");
        cJSON *jweight = cJSON_GetObjectItemCaseSensitive(item, "Weight");
        if (!cJSON_IsString(jaddr) || !cJSON_IsNumber(jweight))
            continue;

        Host h;
        memset(&h, 0, sizeof(h));
        if (parse_addr(jaddr->valuestring, &h) < 0) {
            snprintf(err, errlen, "invalid Addr: %s", jaddr->valuestring);
            backend_free(out);
            cJSON_Delete(root);
            return -1;
        }
        h.weight = jweight->valueint > 0 ? jweight->valueint : 0;
        out->hosts[idx++] = h;
        out->total_weight += h.weight;
    }

    out->nhosts = (size_t)idx;
    cJSON_Delete(root);

    if (out->nhosts == 0) {
        snprintf(err, errlen, "no usable hosts in config");
        backend_free(out);
        return -1;
    }
    return 0;
}

void backend_free(Backend *backend)
{
    if (!backend)
        return;
    free(backend->hosts);
    backend->hosts = NULL;
    backend->nhosts = 0;
    backend->total_weight = 0;
}
