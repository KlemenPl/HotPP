// Default host endpoints. The host listens, agents connect. On Linux an endpoint is
// a Unix socket path; on Windows, a named pipe.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace hotpp {

#ifdef _WIN32
inline constexpr const char* kEndpointSuffix = "";
#else
inline constexpr const char* kEndpointSuffix = ".sock";
#endif

// Every endpoint of this user starts with this. $HOTPP_ENDPOINT_PREFIX overrides it
// (tests use a private one).
// - Linux: $XDG_RUNTIME_DIR is per-user and private; /tmp is the fallback, with the
//   uid in the name.
// - Windows: pipe names are global, so the user name is part of it; the pipes
//   themselves only admit this user.
inline std::string endpoint_prefix() {
    const char* custom = std::getenv("HOTPP_ENDPOINT_PREFIX");
    if (custom && *custom) return custom;
#ifdef _WIN32
    const char* user = std::getenv("USERNAME");
    return std::string("\\\\.\\pipe\\hotpp-") + (user && *user ? user : "user");
#else
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg) + "/hotpp";
    return "/tmp/hotpp-" + std::to_string(getuid());
#endif
}
inline std::string default_endpoint() { return endpoint_prefix() + kEndpointSuffix; }
inline std::string unique_endpoint(uint32_t host_pid) {
    return endpoint_prefix() + "-" + std::to_string(host_pid) + kEndpointSuffix;
}
// The endpoint of the n-th host hotpp mcp (pid `mcp_pid`) launches.
inline std::string launch_endpoint(uint32_t mcp_pid, int n) {
    return endpoint_prefix() + "-mcp-" + std::to_string(mcp_pid) + "-" + std::to_string(n) + kEndpointSuffix;
}

} // namespace hotpp
