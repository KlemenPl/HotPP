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
    // Path to compile_commands.json. Overridden by $HOTPP_COMPILE_COMMANDS. If neither
    // is set, the agent looks in the executable's directory and its parents.
    const char* compile_commands = HOTPP_DEFAULT_COMPILE_COMMANDS;
    // Host endpoint (Unix socket path). Default: $HOTPP_ENDPOINT, then
    // $XDG_RUNTIME_DIR/hotpp.sock (or /tmp/hotpp-<uid>.sock).
    const char* endpoint = nullptr;
    // Host executable to launch when none is listening. Default: $HOTPP_HOST, then "hotpp" on PATH.
    const char* host_path = nullptr;
    // Launch the host if none is listening. $HOTPP_LAUNCH_HOST=0 turns this off.
    bool launch_host = true;
    // Apply patches on the agent thread as soon as they arrive, instead of in
    // sync_point(). For programs without a main loop. $HOTPP_ASYNC=1 turns it on.
    bool async = false;
    // Address space reserved next to the executable for patch code.
    std::size_t region_size = std::size_t(512) << 20;
};

#if defined(HOTPP_DISABLE)
inline bool init(const Config& = Config{}) { return false; }
inline void sync_point() {}
inline void shutdown() {}
inline bool connected() { return false; }
inline bool restarted() { return false; }
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
// True if this process was started by a hot restart (the host rebuilt the program
// and re-executed it). Restore what HOTPP_PRE_RESTART hooks saved.
bool restarted();
#endif

namespace detail {
enum class HookKind { PrePatch, PostPatch, PreRestart };
#if !defined(HOTPP_DISABLE)
void register_hook(HookKind kind, void (*fn)());
#endif
// Stores the hook so the registering initializer writes a variable of its own: a
// patch runs a file's initializers only when they set up new variables
// (docs/DESIGN.md §8.6), and an empty registrar would be optimized away.
struct HookRegistrar {
    void (*fn)();
    HookRegistrar(HookKind kind, void (*f)()) : fn(f) {
#if !defined(HOTPP_DISABLE)
        register_hook(kind, f);
#else
        (void)kind;
#endif
    }
};
} // namespace detail

} // namespace hotpp

#define HOTPP_CONCAT_(a, b) a##b
#define HOTPP_CONCAT(a, b) HOTPP_CONCAT_(a, b)

// Registers `void fn()` to run right before every patch is applied, on the thread
// that applies it (the sync_point() caller, or the agent thread in async mode).
// Use at namespace scope. A hook added or changed by a patch takes effect with it.
#define HOTPP_PRE_PATCH(fn)                                                         \
    [[gnu::used]] static ::hotpp::detail::HookRegistrar HOTPP_CONCAT(hotpp_hook_, __COUNTER__)( \
        ::hotpp::detail::HookKind::PrePatch, fn)

// Registers `void fn()` to run right after every patch is live: rebuild caches,
// re-read data, re-register callbacks.
#define HOTPP_POST_PATCH(fn)                                                        \
    [[gnu::used]] static ::hotpp::detail::HookRegistrar HOTPP_CONCAT(hotpp_hook_, __COUNTER__)( \
        ::hotpp::detail::HookKind::PostPatch, fn)

// Registers `void fn()` to run right before a hot restart replaces the process:
// save whatever state the restarted program should pick up (see restarted()).
#define HOTPP_PRE_RESTART(fn)                                                       \
    [[gnu::used]] static ::hotpp::detail::HookRegistrar HOTPP_CONCAT(hotpp_hook_, __COUNTER__)( \
        ::hotpp::detail::HookKind::PreRestart, fn)
