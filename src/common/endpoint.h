// Default host endpoints. The host listens, agents connect.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace hotpp {

#ifdef _WIN32

inline std::string default_endpoint() { return "\\\\.\\pipe\\hotpp"; }
inline std::string unique_endpoint(uint32_t host_pid) {
    return "\\\\.\\pipe\\hotpp-" + std::to_string(host_pid);
}

#else

// $XDG_RUNTIME_DIR is per-user and private; /tmp is the fallback, with the uid in the name.
inline std::string endpoint_prefix() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg) + "/hotpp";
    return "/tmp/hotpp-" + std::to_string(getuid());
}
inline std::string default_endpoint() { return endpoint_prefix() + ".sock"; }
inline std::string unique_endpoint(uint32_t host_pid) {
    return endpoint_prefix() + "-" + std::to_string(host_pid) + ".sock";
}

#endif

} // namespace hotpp
