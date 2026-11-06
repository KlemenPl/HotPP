// What happened to one patch attempt, for controllers (hotpp mcp) and the event log.
#pragma once

#include "host/util/json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hotpp::host {

struct PatchReport {
    uint32_t id = 0;
    uint32_t pid = 0;
    uint64_t seq = 0;      // event log sequence number it was published at
    std::string trigger;   // "watch" (a saved file) or "request" (a controller asked)
    // applied | compile_failed | link_failed | restart_required | failed_in_app |
    // waiting_for_sync_point | not_applied (anything else, e.g. disconnected)
    std::string status;
    std::vector<std::string> files;   // translation units compiled
    std::vector<std::string> headers; // changed headers that pulled them in
    std::string message;              // the line the host printed
    std::string error;                // why it wasn't applied
    std::string compiler_output;      // ANSI stripped and capped (see cap_output)
    bool compiler_output_truncated = false;
    std::vector<std::string> warnings, notes;
    bool restarting = false; // the host is rebuilding and restarting the program
    int64_t duration_ms = 0;
    uint64_t functions_replaced = 0, functions_new = 0, variables_kept = 0, initializers_run = 0;
    uint64_t bytes = 0;
    uint64_t modules = 0;

    bool applied() const { return status == "applied"; }
    json::Value to_json() const;
};

// `text` without ANSI escape sequences (compilers color their output when CMake
// passes -fdiagnostics-color=always) and without carriage returns.
std::string strip_ansi(std::string_view text);

// At most head + tail bytes of `text`: the start (the first errors matter most) and
// the end, with a marker in between. Cuts at line ends where possible.
std::string cap_output(std::string_view text, size_t head, size_t tail, bool* truncated = nullptr);

} // namespace hotpp::host
