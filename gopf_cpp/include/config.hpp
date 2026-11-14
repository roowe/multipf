#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace gopf {

struct Host {
    std::string host;
    int port = 0;
    int weight = 0;
};

struct Backend {
    std::vector<Host> hosts;
    int total_weight = 0;
};

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/* Parse a gopf JSON config. Throws ConfigError on any invalid input. */
Backend load_config(const std::string& path);

} // namespace gopf
