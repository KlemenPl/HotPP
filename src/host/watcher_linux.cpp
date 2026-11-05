#include "host/watcher.h"

#include "host/compile_db.h"

#include <cerrno>
#include <cstring>
#include <set>
#include <unordered_map>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace hotpp::host {

namespace {

class InotifyWatcher final : public FileWatcher {
public:
    explicit InotifyWatcher(int fd) : fd_(fd) {}
    ~InotifyWatcher() override { ::close(fd_); }

    bool add(const std::filesystem::path& dir, std::string& error) override {
        // IN_CLOSE_WRITE: written in place. IN_MOVED_TO: saved via rename (most editors).
        int wd = inotify_add_watch(fd_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
        if (wd < 0) {
            error = "cannot watch " + dir.string() + ": " + std::strerror(errno);
            return false;
        }
        dirs_[wd] = dir;
        return true;
    }

    std::vector<std::filesystem::path> wait(std::chrono::milliseconds timeout,
                                            std::chrono::milliseconds quiet) override {
        std::set<std::filesystem::path> changed;
        int wait_ms = int(timeout.count());
        while (poll_once(wait_ms, changed)) wait_ms = int(quiet.count());
        return {changed.begin(), changed.end()};
    }

private:
    // True if events arrived within `ms`.
    bool poll_once(int ms, std::set<std::filesystem::path>& changed) {
        pollfd p{fd_, POLLIN, 0};
        int rc = ::poll(&p, 1, ms);
        if (rc <= 0) return false;
        alignas(inotify_event) char buf[16384];
        ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) return false;
        for (char* ptr = buf; ptr < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(ptr);
            auto it = dirs_.find(ev->wd);
            if (it != dirs_.end() && ev->len > 0 && !(ev->mask & IN_ISDIR))
                changed.insert(normalize_path(it->second / ev->name));
            ptr += sizeof(inotify_event) + ev->len;
        }
        return true;
    }

    int fd_;
    std::unordered_map<int, std::filesystem::path> dirs_;
};

} // namespace

std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>& dirs,
                                               std::string& error) {
    int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd < 0) {
        error = std::string("inotify_init1: ") + std::strerror(errno);
        return nullptr;
    }
    auto w = std::make_unique<InotifyWatcher>(fd);
    for (const auto& d : dirs)
        if (!w->add(d, error)) return nullptr;
    return w;
}

} // namespace hotpp::host
