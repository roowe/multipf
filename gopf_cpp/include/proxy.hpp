#pragma once

#include <memory>
#include <string>

#include "backend.hpp"

namespace gopf {

constexpr char WAKE_RELOAD = 1;
constexpr char WAKE_STATUS = 2;
constexpr char WAKE_STOP = 3;

class Proxy {
public:
    Proxy(int port, BackendStore& store, std::string config_path, int wake_fd);
    ~Proxy();

    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    /* Runs until a stop signal arrives. Returns 0 on clean shutdown. */
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gopf
