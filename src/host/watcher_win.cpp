#include "host/watcher.h"

namespace hotpp::host {

std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>&, std::string& error) {
    error = "watching files on Windows arrives in M2";
    return nullptr;
}

} // namespace hotpp::host
