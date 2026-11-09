// The programs connected to a host, as seen from other threads: controllers
// (hotpp mcp) read their state and send them requests through a SessionHandle;
// everything else of a Session stays on its own threads.
#pragma once

#include "host/platform.h"

#include "host/events.h"
#include "host/report.h"
#include "host/util/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hotpp::host {

// A snapshot of one connected program.
struct SessionInfo {
    uint32_t pid = 0;
    uint64_t session = 0; // host-unique; a hot restart gives the same pid a new one
    std::string exe, compile_commands;
    // starting | watching | patching | restarting | unavailable | disconnected
    std::string state = "starting";
    std::string error; // why hot reload is unavailable
    std::vector<std::string> modules;
    uint64_t sources = 0, headers = 0;
    bool scan_done = false;
    uint64_t patches_applied = 0, patches_failed = 0;
    std::shared_ptr<const PatchReport> last_patch;
    std::vector<std::shared_ptr<const PatchReport>> history; // the most recent patches, oldest first
    std::vector<std::string> pending;      // changed but not applied yet (auto-reload off)
    std::vector<std::string> failed_files; // their newest version failed to patch
    bool auto_reload = true;
    bool waiting_for_sync_point = false;
    int64_t connected_ms = 0;

    // The program runs the sources on disk (as far as the host has seen them).
    bool in_sync() const { return pending.empty() && failed_files.empty(); }
    bool ready() const { return state != "starting"; } // set up, one way or the other
    json::Value to_json() const;
};

// A controller's request, carried out on the session's watcher thread.
struct SessionRequest {
    enum class Kind { Reload, Restart };
    explicit SessionRequest(Kind k) : kind(k) {}

    const Kind kind;
    bool rebuild = true; // Restart: run the build command first

    // The first finish() wins; later ones (e.g. a final result after an interim
    // "waiting_for_sync_point") are ignored.
    void finish(std::string outcome, std::shared_ptr<const PatchReport> report = nullptr, std::string error = {},
                std::string output = {});
    // True once finished; waits at most `timeout`.
    bool wait(std::chrono::milliseconds timeout) const;
    bool done() const;

    struct Result {
        std::string outcome;
        std::shared_ptr<const PatchReport> report;
        std::string error, output;
    };
    Result result() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool done_ = false;
    Result result_;
};

class Registry;

class SessionHandle {
public:
    SessionHandle(Registry& registry, uint32_t pid, uint64_t session, bool auto_reload);
    ~SessionHandle();

    uint32_t pid() const { return pid_; }
    uint64_t session() const { return session_; }

    SessionInfo info() const;
    // Changes the snapshot (under its lock) and wakes registry waiters.
    void update(const std::function<void(SessionInfo&)>& change);

    // Queues a request and wakes the watcher thread. False once closed.
    bool post(std::shared_ptr<SessionRequest> request);
    std::vector<std::shared_ptr<SessionRequest>> take_requests();
    // Set when a request is posted or auto-reload changes (the watcher waits for it).
    const WakeEvent& wake_event() const { return wake_; }
    void drain_wake();

    // Stops accepting requests and finishes queued ones with `outcome` / `why`.
    void close(const std::string& outcome, const std::string& why);

    bool auto_reload() const { return auto_reload_.load(); }
    bool default_auto_reload() const { return default_auto_reload_; }
    void set_auto_reload(bool on);

private:
    void wake();

    Registry& registry_;
    const uint32_t pid_;
    const uint64_t session_;
    const bool default_auto_reload_;
    std::atomic<bool> auto_reload_;
    WakeEvent wake_;

    mutable std::mutex mutex_;
    SessionInfo info_;
    std::vector<std::shared_ptr<SessionRequest>> requests_;
    bool closed_ = false;
};

class Registry {
public:
    uint64_t next_session() { return next_session_.fetch_add(1); }

    void add(const std::shared_ptr<SessionHandle>& handle);
    void remove(const SessionHandle* handle);
    // The connected programs: the newest session of each pid, in connection order.
    std::vector<std::shared_ptr<SessionHandle>> list() const;
    // The newest session of `pid`, or nullptr.
    std::shared_ptr<SessionHandle> find(uint32_t pid) const;

    // Waits until `ready()` is true, re-checking whenever a session changes; false
    // on timeout or once `abort` is set. ready() runs without the registry locked.
    bool wait(const std::function<bool()>& ready, std::chrono::milliseconds timeout,
              const std::atomic<bool>* abort = nullptr);
    void notify();

    // Auto-reload set by a controller, for `pid` (and its later sessions, across
    // hot restarts) until the controller lets go of it.
    void set_auto_reload(uint32_t pid, bool on, uint64_t owner);
    void release_auto_reload(uint64_t owner);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t generation_ = 0;
    std::vector<std::shared_ptr<SessionHandle>> sessions_;
    struct Override {
        bool on;
        uint64_t owner;
    };
    std::map<uint32_t, Override> overrides_;
    std::atomic<uint64_t> next_session_{1};
};

} // namespace hotpp::host
