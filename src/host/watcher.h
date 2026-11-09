// Watches directories for saved files.
#pragma once

#include "host/platform.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::host {

class FileWatcher {
public:
    virtual ~FileWatcher() = default;

    // Waits up to `timeout` for files to be written, then keeps collecting until
    // `quiet` passes without another event (editors often write a file in several
    // steps). Returns the changed files, normalized; empty on timeout. Returns early
    // (with what it has) when the wake event given to watch_directories() is set.
    virtual std::vector<std::filesystem::path> wait(std::chrono::milliseconds timeout,
                                                    std::chrono::milliseconds quiet) = 0;
    // Watches one more directory. Call from the thread that calls wait().
    virtual bool add(const std::filesystem::path& dir, std::string& error) = 0;
};

// nullptr (with `error` set) if watching isn't possible. `wake`, if given, interrupts
// wait(); wait() resets it.
std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>& dirs,
                                               std::string& error, const WakeEvent* wake = nullptr);

} // namespace hotpp::host
