#include "backend.hpp"

namespace gopf {

const Host* choose_host(const Backend& backend, std::mt19937_64& rng)
{
    if (backend.hosts.empty() || backend.total_weight <= 0)
        return nullptr;

    std::uniform_int_distribution<int> dist(0, backend.total_weight - 1);
    int pick = dist(rng);

    for (const auto& host : backend.hosts) {
        if (pick < host.weight)
            return &host;
        pick -= host.weight;
    }
    return nullptr;
}

void BackendStore::init(const std::string& path)
{
    backend_ = load_config(path);
}

void BackendStore::reload(const std::string& path)
{
    Backend fresh = load_config(path);
    backend_ = std::move(fresh);
}

} // namespace gopf
