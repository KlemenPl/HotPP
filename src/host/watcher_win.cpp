// File watching on Windows: ReadDirectoryChangesW on each directory, completions
// collected through one I/O completion port.
#include "host/watcher.h"

#include "common/win_util.h"
#include "host/compile_db.h"

#include <memory>
#include <set>

namespace hotpp::host {

namespace {

struct Dir {
    std::filesystem::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    alignas(DWORD) uint8_t buf[32 * 1024];
};

class DirectoryWatcher final : public FileWatcher {
public:
    explicit DirectoryWatcher(const WakeEvent* wake) : wake_(wake) {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    }
    ~DirectoryWatcher() override {
        // Cancel every pending read and wait (boundedly) for the cancellations to come
        // through the port: until then the system may still write to the buffers.
        size_t pending = 0;
        for (auto& d : dirs_)
            if (CancelIoEx(d->handle, &d->ov) || GetLastError() != ERROR_NOT_FOUND) ++pending;
        for (size_t seen = 0; seen < pending;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ov = nullptr;
            if (!GetQueuedCompletionStatus(port_, &bytes, &key, &ov, 2000) && !ov) break; // timed out
            ++seen;
        }
        for (auto& d : dirs_) CloseHandle(d->handle);
        if (port_) CloseHandle(port_);
    }

    bool ok() const { return port_ != nullptr; }

    bool add(const std::filesystem::path& dir, std::string& error) override {
        auto d = std::make_unique<Dir>();
        d->path = dir;
        d->handle = CreateFileW(dir.wstring().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d->handle == INVALID_HANDLE_VALUE) {
            error = win::error_text(("cannot watch " + dir.string()).c_str());
            return false;
        }
        if (!CreateIoCompletionPort(d->handle, port_, ULONG_PTR(dirs_.size()), 0) || !arm(*d)) {
            error = win::error_text(("cannot watch " + dir.string()).c_str());
            CloseHandle(d->handle);
            return false;
        }
        dirs_.push_back(std::move(d));
        return true;
    }

    std::vector<std::filesystem::path> wait(std::chrono::milliseconds timeout, std::chrono::milliseconds quiet) override {
        std::set<std::filesystem::path> changed;
        int wait_ms = int(timeout.count());
        while (poll_once(wait_ms, changed)) wait_ms = int(quiet.count());
        return {changed.begin(), changed.end()};
    }

private:
    bool arm(Dir& d) {
        d.ov = OVERLAPPED{};
        // Writes in place show up as LAST_WRITE/SIZE, saves via rename as FILE_NAME.
        return ReadDirectoryChangesW(d.handle, d.buf, sizeof d.buf, FALSE,
                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE,
                                     nullptr, &d.ov, nullptr) != 0;
    }

    // True if file events arrived within `ms` (and no wake-up). The wake event is
    // checked between short waits on the port.
    bool poll_once(int ms, std::set<std::filesystem::path>& changed) {
        const ULONGLONG deadline = GetTickCount64() + ULONGLONG(ms < 0 ? 0 : ms);
        for (;;) {
            if (wake_ && wake_->wait(0)) {
                wake_->reset();
                drain(changed, 0);
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            const DWORD slice = DWORD(std::min<ULONGLONG>(now >= deadline ? 0 : deadline - now, 20));
            if (drain(changed, slice)) return true;
            if (GetTickCount64() >= deadline) return false;
        }
    }

    // Takes completed notifications, waiting up to `ms` for the first.
    bool drain(std::set<std::filesystem::path>& changed, DWORD ms) {
        bool any = false;
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ov = nullptr;
            if (!GetQueuedCompletionStatus(port_, &bytes, &key, &ov, any ? 0 : ms) || !ov) return any;
            if (key >= dirs_.size()) continue;
            Dir& d = *dirs_[key];
            // bytes == 0: the buffer overflowed and events were lost; a reload's rescan
            // still finds the changes.
            for (DWORD off = 0; bytes && off < bytes;) {
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(d.buf + off);
                if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_MODIFIED ||
                    info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                    const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    const std::filesystem::path p = d.path / name;
                    std::error_code ec;
                    if (!std::filesystem::is_directory(p, ec)) changed.insert(normalize_path(p));
                }
                if (!info->NextEntryOffset) break;
                off += info->NextEntryOffset;
            }
            any = true;
            arm(d);
        }
    }

    const WakeEvent* wake_;
    HANDLE port_ = nullptr;
    std::vector<std::unique_ptr<Dir>> dirs_;
};

} // namespace

std::unique_ptr<FileWatcher> watch_directories(const std::vector<std::filesystem::path>& dirs, std::string& error,
                                               const WakeEvent* wake) {
    auto w = std::make_unique<DirectoryWatcher>(wake);
    if (!w->ok()) {
        error = win::error_text("CreateIoCompletionPort");
        return nullptr;
    }
    for (const auto& d : dirs)
        if (!w->add(d, error)) return nullptr;
    return w;
}

} // namespace hotpp::host
