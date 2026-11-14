#pragma once

#include <random>

#include "config.hpp"

namespace gopf {

/* Weighted random selection. Returns nullptr when no host has weight. */
const Host* choose_host(const Backend& backend, std::mt19937_64& rng);

/* Owns the active backend list. Reloads are transactional: a failed parse
 * leaves the previous configuration untouched. */
class BackendStore {
public:
    void init(const std::string& path);
    void reload(const std::string& path);

    const Backend& get() const { return backend_; }

private:
    Backend backend_;
};

} // namespace gopf
