// HotPP public API. Link hotpp_agent (or use hotpp_enable() in CMake) and call
// hotpp::init() once at startup. Define HOTPP_DISABLE to compile everything to no-ops.
#pragma once

#include <cstddef>

// Set by hotpp_enable() to the build's compile_commands.json.
#ifndef HOTPP_DEFAULT_COMPILE_COMMANDS
#define HOTPP_DEFAULT_COMPILE_COMMANDS nullptr
#endif

namespace hotpp {

struct Config {
    // Path to compile_commands.json. Overridden by $HOTPP_COMPILE_COMMANDS.
    const char* compile_commands = HOTPP_DEFAULT_COMPILE_COMMANDS;
    // Host endpoint (Unix socket path / named pipe name). Default: $HOTPP_ENDPOINT,
    // then $XDG_RUNTIME_DIR/hotpp.sock (Linux) or \\.\pipe\hotpp (Windows).
    const char* endpoint = nullptr;
    // Host executable to launch when none is listening. Default: $HOTPP_HOST, then "hotpp" on PATH.
    const char* host_path = nullptr;
    // Launch the host if none is listening. $HOTPP_LAUNCH_HOST=0 turns this off.
    bool launch_host = true;
    // Apply patches as soon as they arrive instead of at sync_point(). (Not implemented yet.)
    bool async = false;
    // Address space reserved next to the executable for patch code.
    std::size_t region_size = std::size_t(512) << 20;
};

#if defined(HOTPP_DISABLE)
inline bool init(const Config& = Config{}) { return false; }
inline void sync_point() {}
inline void shutdown() {}
inline bool connected() { return false; }
#else
// Connects to the host. Returns false (and the app keeps running normally) if hot
// reload is disabled ($HOTPP_DISABLE=1) or unavailable. Call once per process.
bool init(const Config& config = Config{});
// Applies pending patches. Call from a point where no patched code is mid-update,
// typically the top of the main loop.
void sync_point();
// Disconnects from the host. Patch memory stays mapped until the process exits.
void shutdown();
// True while connected to a host.
bool connected();
#endif

} // namespace hotpp
