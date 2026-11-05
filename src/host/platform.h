// OS layer of the host. One implementation per platform: platform_linux.cpp, platform_win.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::host {

// One agent connection. send/recv transfer exactly `size` bytes or fail.
class Stream {
public:
    virtual ~Stream() = default;
    virtual bool send(const void* data, size_t size) = 0;
    virtual bool recv(void* data, size_t size) = 0;
};

class Listener {
public:
    virtual ~Listener() = default;
    // Blocks until an agent connects; nullptr once the listener is closed or broken.
    virtual std::unique_ptr<Stream> accept() = 0;
    // Stops accepting and removes the endpoint. Safe to call from another thread.
    virtual void close() = 0;
};

std::unique_ptr<Listener> listen(const std::string& endpoint, std::string& error);

// Runs argv[0] (searched on PATH) with the host's stdio, waits for it and returns
// its exit code (128 + signal number if killed by a signal), or -1 if it couldn't start.
int run_child(const std::vector<std::string>& argv, std::string& error);

bool set_env(const char* name, const std::string& value);
uint32_t current_pid();
void init_console(); // UTF-8 console output where needed

} // namespace hotpp::host
