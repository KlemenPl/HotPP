// What a host session has patched into a process, saved after every patch so a
// restarted host can carry on where the previous one stopped (docs/DESIGN.md §3.3).
#pragma once

#include "host/link/symbol_index.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hotpp::host {

struct SavedModule {
    uint32_t index = 0; // into the hello's module list
    std::string path;
    std::vector<uint8_t> build_id;
    uint64_t region_base = 0;
    uint64_t cursor = 0; // first free byte of its patch region
};

struct SessionState {
    uint32_t pid = 0;
    uint32_t next_patch = 1;
    std::vector<SavedModule> modules;
    std::vector<std::pair<uint32_t, link::IndexDelta>> deltas; // (module index, delta), in order
};

// Atomic (write + rename). False on I/O errors.
bool save_session_state(const std::filesystem::path& path, const SessionState& state);
// nullopt if missing or unreadable.
std::optional<SessionState> load_session_state(const std::filesystem::path& path);

} // namespace hotpp::host
