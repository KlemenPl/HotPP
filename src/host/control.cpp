#include "host/control.h"

#include "host/control_protocol.h"

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

#include <unistd.h>

namespace hotpp::host {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

json::Value HostContext::host_json() const {
    json::Value h = json::Value::object();
    h.set("version", HOTPP_VERSION).set("pid", int64_t(::getpid())).set("endpoint", endpoint);
    h.set("mode", run_mode ? "run" : "serve").set("launched_by_mcp", lifeline);
    h.set("build_command", options.build_command).set("auto_restart", options.auto_restart);
    h.set("auto_reload", options.auto_reload).set("captures_output", output != nullptr);
    if (run_mode) {
        std::lock_guard<std::mutex> lock(child.mutex);
        json::Value c = json::Value::object();
        json::Value cmd = json::Value::array();
        for (const std::string& a : child.command) cmd.push(a);
        // Not started yet counts as running: it's about to.
        c.set("pid", child.pid).set("command", std::move(cmd)).set("running", child.running || child.pid == 0);
        if (!child.running && child.pid) c.set("exit_code", child.exit_code);
        h.set("child", std::move(c));
    }
    return h;
}

namespace {

struct ControlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

int64_t int_param(const json::Value& p, const char* name, int64_t fallback, int64_t lo, int64_t hi) {
    const json::Value* v = p.get(name);
    if (!v || v->is_null()) return fallback;
    if (!v->is_number()) throw ControlError(std::string("'") + name + "' must be a number");
    const double d = v->as_number();
    if (d < double(lo) || d > double(hi))
        throw ControlError(std::string("'") + name + "' must be between " + std::to_string(lo) + " and " +
                           std::to_string(hi));
    return int64_t(d);
}

bool bool_param(const json::Value& p, const char* name, bool fallback, bool required = false) {
    const json::Value* v = p.get(name);
    if (!v || v->is_null()) {
        if (required) throw ControlError(std::string("'") + name + "' is required");
        return fallback;
    }
    if (!v->is_bool()) throw ControlError(std::string("'") + name + "' must be true or false");
    return v->as_bool();
}

constexpr int64_t kMaxTimeoutMs = 600000;

class Controller {
public:
    Controller(Stream& stream, HostContext& ctx) : stream_(stream), ctx_(ctx), owner_(ctx.next_controller++) {}

    void run() {
        json::Value hello = json::Value::object();
        hello.set("hello", ctx_.host_json()).set("control_version", kControlVersion);
        if (!send(hello)) return;
        for (;;) {
            proto::MsgType type{};
            json::Value msg;
            const RecvStatus st = recv_json(stream_, type, msg, kMaxControlRequest);
            if (st == RecvStatus::Closed) break;
            if (st == RecvStatus::Malformed || type != proto::MsgType::Control || !msg.is_object()) {
                reply_error(json::Value(), "malformed control message");
                continue;
            }
            reap();
            if (int(workers_.size()) >= kMaxRequestsInFlight) {
                reply_error(msg.get("id") ? *msg.get("id") : json::Value(), "too many requests in flight");
                continue;
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            workers_.push_back({std::thread([this, msg = std::move(msg), done] {
                                    handle(msg);
                                    *done = true;
                                }),
                                done});
        }
        // The controller is gone (or the host is shutting down): end waits early,
        // let replies in progress go out, then let go of its auto-reload settings.
        closing_ = true;
        ctx_.events.notify();
        ctx_.registry.notify();
        {
            std::lock_guard<std::mutex> lock(ctx_.child.mutex); // no waiter misses closing_
        }
        ctx_.child.cv.notify_all();
        for (Worker& w : workers_) w.thread.join();
        workers_.clear();
        ctx_.registry.release_auto_reload(owner_);
    }

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    bool send(const json::Value& v) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        return send_json(stream_, proto::MsgType::Control, v);
    }

    void reply_error(const json::Value& id, const std::string& message) {
        json::Value e = json::Value::object();
        e.set("message", message);
        json::Value r = json::Value::object();
        r.set("id", id).set("error", std::move(e));
        send(r);
    }

    void reap() {
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (!*it->done) {
                ++it;
                continue;
            }
            it->thread.join();
            it = workers_.erase(it);
        }
    }

    void handle(const json::Value& msg) {
        const json::Value* id = msg.get("id");
        const json::Value* method = msg.get("method");
        if (!id || !id->is_integer() || !method || !method->is_string()) {
            reply_error(id ? *id : json::Value(), "a request needs an integer 'id' and a string 'method'");
            return;
        }
        static const json::Value kNoParams = json::Value::object();
        const json::Value* params = msg.get("params");
        if (params && !params->is_object() && !params->is_null()) {
            reply_error(*id, "'params' must be an object");
            return;
        }
        if (!params || params->is_null()) params = &kNoParams;
        try {
            json::Value result = call(method->as_string(), *params, *id);
            json::Value r = json::Value::object();
            r.set("id", *id).set("result", std::move(result));
            send(r);
        } catch (const ControlError& e) {
            reply_error(*id, e.what());
        } catch (const std::exception& e) {
            reply_error(*id, std::string("internal error: ") + e.what());
        }
    }

    json::Value call(const std::string& method, const json::Value& p, const json::Value& id) {
        if (method == "status") return status();
        if (method == "reload") return reload(p, id);
        if (method == "restart") return restart(p, id);
        if (method == "events") return events(p);
        if (method == "output") return output(p);
        if (method == "set_auto_reload") return set_auto_reload(p);
        if (method == "wait_program") return wait_program(p);
        if (method == "stop") return stop(p);
        throw ControlError("unknown method '" + method + "'");
    }

    // The program `p` names with "pid", or the only one connected.
    std::shared_ptr<SessionHandle> program(const json::Value& p) {
        if (p.get("pid") && !p.get("pid")->is_null()) {
            const uint32_t pid = uint32_t(int_param(p, "pid", 0, 1, INT32_MAX));
            if (auto h = ctx_.registry.find(pid)) return h;
            throw ControlError("no program with pid " + std::to_string(pid) + " is connected to this host");
        }
        const auto all = ctx_.registry.list();
        std::set<uint32_t> pids;
        for (const auto& h : all) pids.insert(h->pid());
        if (pids.empty()) throw ControlError("no program is connected to this host");
        if (pids.size() > 1) {
            std::string list;
            for (uint32_t pid : pids) list += (list.empty() ? "" : ", ") + std::to_string(pid);
            throw ControlError("several programs are connected (pids " + list + "); say which with 'pid'");
        }
        return ctx_.registry.find(*pids.begin());
    }

    bool child_running() const {
        std::lock_guard<std::mutex> lock(ctx_.child.mutex);
        return ctx_.child.running;
    }
    // Started and gone. Not the same as !child_running(): a controller can connect
    // before the host has started its program.
    bool child_exited() const {
        std::lock_guard<std::mutex> lock(ctx_.child.mutex);
        return ctx_.child.pid != 0 && !ctx_.child.running;
    }

    // Waits for `req`, passing the program's log lines on as progress. False on
    // timeout or when the controller goes away.
    bool wait_with_progress(SessionRequest& req, uint32_t pid, Clock::time_point deadline, const json::Value& id) {
        uint64_t seq = ctx_.events.last_seq();
        for (;;) {
            if (req.wait(100ms)) return true;
            const EventLog::Page page = ctx_.events.after(seq, pid, 50, 0ms);
            seq = page.next;
            for (const Event& e : page.events) {
                if (e.kind != "log") continue;
                json::Value m = json::Value::object();
                m.set("id", id).set("progress", e.text);
                send(m);
            }
            if (closing_ || Clock::now() >= deadline) return req.done();
        }
    }

    // The next session of `pid` after `after` once it's set up (or the program
    // exited, or the deadline passed): nullptr unless it came back.
    std::shared_ptr<SessionHandle> wait_for_return(uint32_t pid, uint64_t after, Clock::time_point deadline) {
        std::shared_ptr<SessionHandle> found;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        ctx_.registry.wait(
            [&] {
                auto h = ctx_.registry.find(pid);
                if (h && h->session() > after && h->info().ready()) {
                    found = h;
                    return true;
                }
                return ctx_.run_mode && child_exited();
            },
            std::max(left, 0ms), &closing_);
        return found;
    }

    json::Value status() {
        json::Value programs = json::Value::array();
        for (const auto& h : ctx_.registry.list()) programs.push(h->info().to_json());
        json::Value r = json::Value::object();
        r.set("host", ctx_.host_json()).set("programs", std::move(programs));
        return r;
    }

    // Patches recorded for `pid` (in the sessions given) after `since`, oldest first.
    static json::Value recent(const std::vector<std::shared_ptr<SessionHandle>>& sessions, uint64_t since,
                              uint64_t skip) {
        std::map<uint64_t, std::shared_ptr<const PatchReport>> by_seq;
        for (const auto& h : sessions)
            if (h)
                for (const auto& r : h->info().history)
                    if (r->seq > since && r->seq != skip) by_seq[r->seq] = r;
        json::Value a = json::Value::array();
        for (const auto& [seq, r] : by_seq) a.push(r->to_json());
        return a;
    }

    json::Value reload(const json::Value& p, const json::Value& id) {
        auto h = program(p);
        const uint32_t pid = h->pid();
        const auto deadline = Clock::now() + std::chrono::milliseconds(int_param(p, "timeout_ms", 60000, 0, kMaxTimeoutMs));
        const uint64_t since = uint64_t(int_param(p, "since_seq", 0, 0, INT64_MAX));
        const SessionInfo before = h->info();
        if (before.state == "unavailable")
            throw ControlError("hot reload is unavailable for pid " + std::to_string(pid) + ": " + before.error);
        auto req = std::make_shared<SessionRequest>(SessionRequest::Kind::Reload);
        if (!h->post(req)) {
            // Restarting (its next session takes requests) or gone.
            auto next = wait_for_return(pid, h->session(), deadline);
            if (!next || !next->post(req))
                throw ControlError("pid " + std::to_string(pid) + " is " +
                                   (before.state == "restarting" ? "restarting" : "no longer connected"));
            h = next;
        }
        json::Value r = json::Value::object();
        std::shared_ptr<SessionHandle> latest = h;
        uint64_t skip = 0;
        if (!wait_with_progress(*req, pid, deadline, id)) {
            r.set("outcome", closing_ ? "cancelled" : "timeout");
        } else {
            const SessionRequest::Result res = req->result();
            r.set("outcome", res.outcome);
            if (!res.error.empty()) r.set("error", res.error);
            if (res.report) {
                r.set("report", res.report->to_json());
                skip = res.report->seq;
                if (res.report->restarting) {
                    auto next = wait_for_return(pid, h->session(), deadline);
                    r.set("restarted", next != nullptr);
                    if (next) latest = next;
                }
            }
        }
        if (auto now = ctx_.registry.find(pid)) latest = now;
        const SessionInfo info = latest->info();
        r.set("recent", recent({h, latest}, since, skip));
        r.set("in_sync", info.in_sync()).set("program", info.to_json());
        return r;
    }

    json::Value restart(const json::Value& p, const json::Value& id) {
        auto h = program(p);
        const uint32_t pid = h->pid();
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(int_param(p, "timeout_ms", 300000, 0, kMaxTimeoutMs));
        auto req = std::make_shared<SessionRequest>(SessionRequest::Kind::Restart);
        req->rebuild = bool_param(p, "rebuild", true);
        const SessionInfo before = h->info();
        if (before.state == "unavailable" || !h->post(req))
            throw ControlError("pid " + std::to_string(pid) + " can't be restarted now (" + before.state +
                               (before.error.empty() ? "" : ": " + before.error) + ")");
        json::Value r = json::Value::object();
        if (!wait_with_progress(*req, pid, deadline, id)) {
            r.set("outcome", closing_ ? "cancelled" : "timeout");
        } else {
            const SessionRequest::Result res = req->result();
            r.set("outcome", res.outcome);
            if (!res.error.empty()) r.set("error", res.error);
            if (!res.output.empty()) r.set("build_output", res.output);
            if (res.outcome == "restarting") {
                if (auto next = wait_for_return(pid, h->session(), deadline)) {
                    r.set("outcome", "restarted");
                    r.set("program", next->info().to_json());
                } else if (ctx_.run_mode && child_exited()) {
                    r.set("outcome", "exited");
                    r.set("error", "the program exited instead of coming back");
                } else {
                    r.set("outcome", closing_ ? "cancelled" : "timeout");
                    r.set("error", "the program didn't reconnect in time");
                }
            }
        }
        if (!r.get("program"))
            if (auto now = ctx_.registry.find(pid)) r.set("program", now->info().to_json());
        r.set("host", ctx_.host_json());
        return r;
    }

    json::Value events(const json::Value& p) {
        const uint32_t pid = uint32_t(int_param(p, "pid", 0, 0, INT32_MAX));
        const size_t limit = size_t(int_param(p, "limit", 100, 1, 1000));
        const auto wait = std::chrono::milliseconds(int_param(p, "wait_ms", 0, 0, kMaxTimeoutMs));
        const json::Value* since = p.get("since");
        EventLog::Page page = since && !since->is_null()
                                  ? ctx_.events.after(uint64_t(int_param(p, "since", 0, 0, INT64_MAX)), pid, limit, wait,
                                                      &closing_)
                                  : ctx_.events.tail(pid, limit);
        json::Value list = json::Value::array();
        for (const Event& e : page.events) list.push(e.to_json());
        json::Value r = json::Value::object();
        r.set("events", std::move(list)).set("next", page.next).set("truncated", page.truncated);
        return r;
    }

    json::Value output(const json::Value& p) {
        if (!ctx_.output)
            throw ControlError("this host doesn't capture the program's output (only programs started with "
                               "hotpp_launch do)");
        const size_t max_bytes = size_t(int_param(p, "max_bytes", 16384, 1, 1 << 20));
        const size_t lines = size_t(int_param(p, "tail_lines", 0, 0, 100000));
        const auto wait = std::chrono::milliseconds(int_param(p, "wait_ms", 0, 0, kMaxTimeoutMs));
        const json::Value* since = p.get("since");
        const bool from = since && !since->is_null();
        // With wait_ms: what the program prints during the wait (from `since`, or from
        // now), returned early if it exits.
        uint64_t start = from ? uint64_t(int_param(p, "since", 0, 0, INT64_MAX)) : ctx_.output->end();
        if (wait.count() > 0) {
            std::unique_lock<std::mutex> lock(ctx_.child.mutex);
            ctx_.child.cv.wait_for(lock, wait, [&] { return !ctx_.child.running || closing_.load(); });
        }
        const OutputBuffer::Chunk c = from || wait.count() > 0 ? ctx_.output->read(start, max_bytes)
                                                               : ctx_.output->tail(lines, max_bytes);
        json::Value r = json::Value::object();
        r.set("text", strip_ansi(c.text)).set("start", c.start).set("next", c.next).set("dropped", c.dropped);
        r.set("closed", c.closed).set("host", ctx_.host_json());
        return r;
    }

    json::Value set_auto_reload(const json::Value& p) {
        auto h = program(p);
        const bool on = bool_param(p, "enabled", false, true);
        ctx_.registry.set_auto_reload(h->pid(), on, owner_);
        json::Value r = json::Value::object();
        r.set("pid", h->pid()).set("auto_reload", on).set("program", h->info().to_json());
        return r;
    }

    json::Value wait_program(const json::Value& p) {
        const auto timeout = std::chrono::milliseconds(int_param(p, "timeout_ms", 30000, 0, kMaxTimeoutMs));
        std::shared_ptr<SessionHandle> ready;
        ctx_.registry.wait(
            [&] {
                for (const auto& h : ctx_.registry.list())
                    if (h->info().ready()) {
                        ready = h;
                        return true;
                    }
                return ctx_.run_mode && child_exited();
            },
            timeout, &closing_);
        json::Value r = json::Value::object();
        r.set("ready", ready != nullptr);
        if (ready) r.set("program", ready->info().to_json());
        r.set("host", ctx_.host_json());
        return r;
    }

    // Stops the program (if it still runs) and lets the host exit.
    json::Value stop(const json::Value& p) {
        if (!ctx_.lifeline || !ctx_.stop_child)
            throw ControlError("only programs started by hotpp mcp can be stopped through the control channel");
        const auto timeout = std::chrono::milliseconds(int_param(p, "timeout_ms", 10000, 0, kMaxTimeoutMs));
        const bool was_running = child_running();
        if (was_running) ctx_.stop_child(std::max(std::min(timeout / 2, std::chrono::milliseconds(5000)), 100ms));
        json::Value r = json::Value::object();
        {
            std::unique_lock<std::mutex> lock(ctx_.child.mutex);
            const bool stopped =
                ctx_.child.cv.wait_for(lock, timeout, [&] { return !ctx_.child.running || closing_.load(); }) &&
                !ctx_.child.running;
            r.set("was_running", was_running).set("stopped", stopped);
            if (stopped) {
                r.set("exit_code", ctx_.child.exit_code);
                ctx_.child.dismissed = true;
            }
        }
        ctx_.child.cv.notify_all();
        return r;
    }

    Stream& stream_;
    HostContext& ctx_;
    const uint64_t owner_;
    std::mutex send_mutex_;
    std::atomic<bool> closing_{false};
    std::list<Worker> workers_;
};

} // namespace

void serve_controller(Stream& stream, const json::Value& hello, HostContext& ctx) {
    const json::Value* version = hello.get("control_version");
    if (!version || !version->is_number() || version->as_number() < 1) {
        json::Value e = json::Value::object();
        e.set("message", "unsupported control protocol; this host speaks version " + std::to_string(kControlVersion));
        json::Value r = json::Value::object();
        r.set("error", std::move(e));
        send_json(stream, proto::MsgType::Control, r);
        return;
    }
    Controller(stream, ctx).run();
}

} // namespace hotpp::host
