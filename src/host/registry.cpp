#include "host/registry.h"

#include <algorithm>


namespace hotpp::host {

namespace {
json::Value strings(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (const std::string& s : v) a.push(s);
    return a;
}
} // namespace

json::Value SessionInfo::to_json() const {
    json::Value v = json::Value::object();
    v.set("pid", pid).set("session", session).set("exe", exe).set("compile_commands", compile_commands);
    v.set("state", state);
    if (!error.empty()) v.set("error", error);
    v.set("modules", strings(modules));
    v.set("sources", sources).set("headers", headers).set("dependency_scan_done", scan_done);
    v.set("patches_applied", patches_applied).set("patches_failed", patches_failed);
    v.set("auto_reload", auto_reload).set("in_sync", in_sync());
    v.set("pending", strings(pending)).set("failed_files", strings(failed_files));
    v.set("waiting_for_sync_point", waiting_for_sync_point);
    v.set("connected_ms", connected_ms);
    if (last_patch) {
        json::Value p = last_patch->to_json();
        p.set("compiler_output", nullptr); // in the full report (reload / events), not in every status
        v.set("last_patch", std::move(p));
    }
    return v;
}

void SessionRequest::finish(std::string outcome, std::shared_ptr<const PatchReport> report, std::string error,
                            std::string output) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_) return;
        done_ = true;
        result_.outcome = std::move(outcome);
        result_.report = std::move(report);
        result_.error = std::move(error);
        result_.output = std::move(output);
    }
    cv_.notify_all();
}

bool SessionRequest::wait(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return done_; });
}

bool SessionRequest::done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
}

SessionRequest::Result SessionRequest::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

SessionHandle::SessionHandle(Registry& registry, uint32_t pid, uint64_t session, bool auto_reload)
    : registry_(registry), pid_(pid), session_(session), default_auto_reload_(auto_reload),
      auto_reload_(auto_reload) {
    info_.pid = pid;
    info_.session = session;
    info_.auto_reload = auto_reload;
}

SessionHandle::~SessionHandle() = default;

SessionInfo SessionHandle::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionInfo copy = info_;
    copy.auto_reload = auto_reload_.load();
    return copy;
}

void SessionHandle::update(const std::function<void(SessionInfo&)>& change) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        change(info_);
    }
    registry_.notify();
}

bool SessionHandle::post(std::shared_ptr<SessionRequest> request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        requests_.push_back(std::move(request));
    }
    wake();
    return true;
}

std::vector<std::shared_ptr<SessionRequest>> SessionHandle::take_requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(requests_);
}

void SessionHandle::wake() { wake_.set(); }

void SessionHandle::drain_wake() { wake_.reset(); }

void SessionHandle::close(const std::string& outcome, const std::string& why) {
    std::vector<std::shared_ptr<SessionRequest>> queued;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        queued = std::move(requests_);
    }
    for (auto& r : queued) r->finish(outcome, nullptr, why);
    registry_.notify();
}

void SessionHandle::set_auto_reload(bool on) {
    if (auto_reload_.exchange(on) != on) {
        update([&](SessionInfo& i) { i.auto_reload = on; });
        wake();
    }
}

void Registry::add(const std::shared_ptr<SessionHandle>& handle) {
    std::optional<bool> override_value;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.push_back(handle);
        auto it = overrides_.find(handle->pid());
        if (it != overrides_.end()) override_value = it->second.on;
        ++generation_;
    }
    if (override_value) handle->set_auto_reload(*override_value);
    cv_.notify_all();
}

void Registry::remove(const SessionHandle* handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const auto& s) { return s.get() == handle; }),
                        sessions_.end());
        ++generation_;
    }
    cv_.notify_all();
}

std::vector<std::shared_ptr<SessionHandle>> Registry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // After a hot restart the old session lingers until its threads end; the
    // newest one is the program.
    std::map<uint32_t, std::shared_ptr<SessionHandle>> newest;
    for (const auto& s : sessions_) {
        auto& n = newest[s->pid()];
        if (!n || s->session() > n->session()) n = s;
    }
    std::vector<std::shared_ptr<SessionHandle>> out;
    for (const auto& s : sessions_)
        if (newest[s->pid()] == s) out.push_back(s);
    return out;
}

std::shared_ptr<SessionHandle> Registry::find(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<SessionHandle> best;
    for (const auto& s : sessions_)
        if (s->pid() == pid && (!best || s->session() > best->session())) best = s;
    return best;
}

bool Registry::wait(const std::function<bool()>& ready, std::chrono::milliseconds timeout,
                    const std::atomic<bool>* abort) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        uint64_t seen;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seen = generation_;
        }
        if (ready()) return true;
        if (abort && abort->load()) return false;
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_until(lock, deadline, [&] { return generation_ != seen || (abort && abort->load()); })) {
            lock.unlock();
            return ready();
        }
    }
}

void Registry::notify() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
    }
    cv_.notify_all();
}

void Registry::set_auto_reload(uint32_t pid, bool on, uint64_t owner) {
    std::vector<std::shared_ptr<SessionHandle>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[pid] = Override{on, owner};
        for (const auto& s : sessions_)
            if (s->pid() == pid) targets.push_back(s);
    }
    for (const auto& s : targets) s->set_auto_reload(on);
}

void Registry::release_auto_reload(uint64_t owner) {
    std::vector<std::shared_ptr<SessionHandle>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = overrides_.begin(); it != overrides_.end();) {
            if (it->second.owner != owner) {
                ++it;
                continue;
            }
            for (const auto& s : sessions_)
                if (s->pid() == it->first) targets.push_back(s);
            it = overrides_.erase(it);
        }
    }
    for (const auto& s : targets) s->set_auto_reload(s->default_auto_reload());
}

} // namespace hotpp::host
