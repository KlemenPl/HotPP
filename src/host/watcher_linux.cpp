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
    InotifyWatcher(int fd, int wake_fd) : fd_(fd), wake_fd_(wake_fd) {}
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
    // True if file events arrived within `ms` (and no wake-up).
    bool poll_once(int ms, std::set<std::filesystem::path>& changed) {
        pollfd p[2] = {{fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
        int rc = ::poll(p, wake_fd_ >= 0 ? 2 : 1, ms);
        if (rc <= 0) return false;
        if (wake_fd_ >= 0 && (p[1].revents & POLLIN)) {
            uint64_t v;
            ssize_t r = ::read(wake_fd_, &v, sizeof v);
            (void)r;
            if (!(p[0].revents & POLLIN)) return false;
            read_events(changed);
            return false;
        }
        if (!(p[0].revents & POLLIN)) return false;
        read_events(changed);
        return true;
    }

    void read_events(std::set<std::filesystem::path>& changed) {
        alignas(inotify_event) char buf[16384];
        for (;;) {
            ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n <= 0) return; // EAGAIN: all read
            for (char* ptr = buf; ptr < buf + n;) {
                auto* ev = reinterpret_cast<inotify_event*>(ptr);
                auto it = dirs_.find(ev->wd);
                if (it != dirs_.end() && ev->len > 0 && !(ev->mask & IN_ISDIR))
                    changed.insert(normalize_path(it->second / ev->name));
                ptr += sizeof(inotify_event) + ev->len;
            }
        }
    }

    int fd_;
    int wake_fd_;
    std::unordered_map<int, std::filesystem::path> dirs_;
};

} // namespace

std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>& dirs,
                                               std::string& error, int wake_fd) {
    int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd < 0) {
        error = std::string("inotify_init1: ") + std::strerror(errno);
        return nullptr;
    }
    auto w = std::make_unique<InotifyWatcher>(fd, wake_fd);
    for (const auto& d : dirs)
        if (!w->add(d, error)) return nullptr;
    return w;
}

} // namespace hotpp::host
