// Watches directories for saved files.
#pragma once

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
    // steps). Returns the changed files, normalized; empty on timeout.
    virtual std::vector<std::filesystem::path> wait(std::chrono::milliseconds timeout,
                                                    std::chrono::milliseconds quiet) = 0;
};

// nullptr (with `error` set) if watching isn't possible.
std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>& dirs,
                                               std::string& error);

} // namespace hotpp::host
