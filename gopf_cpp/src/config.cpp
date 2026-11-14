#include "config.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#include "json.hpp"

namespace gopf {
namespace {

Host parse_addr(const std::string& addr)
{
    const auto colon = addr.rfind(':');
    if (colon == std::string::npos)
        throw ConfigError("invalid Addr: " + addr);

    Host host;
    host.host = addr.substr(0, colon);
    if (host.host.empty())
        host.host = "127.0.0.1";

    int port = 0;
    try {
        port = std::stoi(addr.substr(colon + 1));
    } catch (const std::exception&) {
        throw ConfigError("invalid port in Addr: " + addr);
    }
    if (port <= 0 || port > 65535)
        throw ConfigError("invalid port in Addr: " + addr);

    host.port = port;
    return host;
}

} // namespace

Backend load_config(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        throw ConfigError("open " + path + ": " + std::strerror(errno));

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception&) {
        throw ConfigError("invalid JSON in " + path);
    }

    if (!doc.contains("Hosts") || !doc["Hosts"].is_array())
        throw ConfigError("missing Hosts array");

    Backend backend;
    for (const auto& item : doc["Hosts"]) {
        if (!item.contains("Addr") || !item["Addr"].is_string())
            continue;
        if (!item.contains("Weight") || !item["Weight"].is_number())
            continue;

        Host host = parse_addr(item["Addr"].get<std::string>());
        host.weight = std::max(0, item["Weight"].get<int>());
        backend.total_weight += host.weight;
        backend.hosts.push_back(std::move(host));
    }

    if (backend.hosts.empty())
        throw ConfigError("no usable hosts in config");

    return backend;
}

} // namespace gopf
