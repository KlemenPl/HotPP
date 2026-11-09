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

// An environment variable, or "" if unset.
inline std::string env_value(const char* name) {
#ifdef _WIN32
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return {};
    std::string s = v;
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

// Every endpoint of this user starts with this. $HOTPP_ENDPOINT_PREFIX overrides it
// (tests use a private one).
// - Linux: $XDG_RUNTIME_DIR is per-user and private; /tmp is the fallback, with the
//   uid in the name.
// - Windows: pipe names are global, so the user name is part of it; the pipes
//   themselves only admit this user.
inline std::string endpoint_prefix() {
    const std::string custom = env_value("HOTPP_ENDPOINT_PREFIX");
    if (!custom.empty()) return custom;
#ifdef _WIN32
    const std::string user = env_value("USERNAME");
    return std::string("\\\\.\\pipe\\hotpp-") + (user.empty() ? "user" : user);
#else
    const std::string xdg = env_value("XDG_RUNTIME_DIR");
    if (!xdg.empty()) return xdg + "/hotpp";
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
