// A host's state shared by its connections, and serving controllers (hotpp mcp)
// on the control channel (control_protocol.h).
#pragma once

#include "host/events.h"
#include "host/platform.h"
#include "host/registry.h"
#include "host/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hotpp::host {

// The program a host started (run mode).
struct ChildProcess {
    mutable std::mutex mutex;
    std::condition_variable cv;
    int pid = 0;
    bool running = false;
    int exit_code = -1;
    bool dismissed = false; // a controller is done with it: the host may exit (with --lifeline)
    std::vector<std::string> command;
};

struct HostContext {
    HostOptions options;
    std::string endpoint;
    bool run_mode = false;
    bool lifeline = false; // started by hotpp mcp: controllers may stop the program
    EventLog events;
    Registry registry;
    std::unique_ptr<OutputBuffer> output; // the program's output, with --capture-output
    ChildProcess child;
    // Sends the program SIGTERM, and SIGKILL if it's still running after `grace`.
    std::function<void(std::chrono::milliseconds grace)> stop_child;
    std::atomic<int> controllers{0};
    std::atomic<uint64_t> next_controller{1};

    json::Value host_json() const;
};

// Serves one controller whose ControlHello payload was `hello`, until it disconnects
// or the host shuts down (Stream::shutdown_read() ends it after its replies).
void serve_controller(Stream& stream, const json::Value& hello, HostContext& ctx);

} // namespace hotpp::host
