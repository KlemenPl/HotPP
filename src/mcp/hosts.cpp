#include "mcp/hosts.h"

#include "common/endpoint.h"
#include "host/control_protocol.h"
#include "host/platform.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace hotpp::mcp {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ---- HostClient --------------------------------------------------------------

std::shared_ptr<HostClient> HostClient::connect(const std::string& endpoint, std::chrono::milliseconds timeout,
                                                std::string& error, bool& incompatible) {
    incompatible = false;
    std::shared_ptr<HostClient> c(new HostClient);
    c->endpoint_ = endpoint;
    c->stream_ = host::connect(endpoint, error);
    if (!c->stream_) return nullptr;
    json::Value hello = json::Value::object();
    hello.set("control_version", host::kControlVersion).set("client", std::string("hotpp mcp ") + HOTPP_VERSION);
    if (!host::send_json(*c->stream_, proto::MsgType::ControlHello, hello)) {
        error = "can't talk to " + endpoint;
        return nullptr;
    }
    c->reader_ = std::thread(&HostClient::read_loop, c.get());
    bool answered;
    {
        std::unique_lock<std::mutex> lock(c->mutex_);
        answered = c->hello_cv_.wait_for(lock, timeout, [&] { return c->got_hello_ || !c->alive_; });
        answered = answered && c->got_hello_;
    }
    if (!answered) {
        // Closed on us without a word: something that doesn't speak the control
        // protocol, e.g. a HotPP 1.0 host.
        incompatible = !c->alive_;
        error = incompatible ? "it doesn't accept controllers (a HotPP host older than 1.1?)"
                             : "no answer from " + endpoint;
        c->close();
        return nullptr;
    }
    if (!c->hello_.is_object()) {
        incompatible = true;
        error = "it refused the connection";
        if (const json::Value* m = c->hello_.get("message"); m && m->is_string()) error += ": " + m->as_string();
        c->close();
        return nullptr;
    }
    return c;
}

HostClient::~HostClient() { close(); }

void HostClient::close() {
    if (stream_) stream_->shutdown();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
}

int64_t HostClient::host_pid() const {
    const json::Value* p = hello_.get("pid");
    return p && p->is_integer() ? p->as_integer() : -1;
}

void HostClient::read_loop() {
    for (;;) {
        proto::MsgType type{};
        json::Value v;
        const host::RecvStatus st = host::recv_json(*stream_, type, v, host::kMaxControlReply);
        if (st == host::RecvStatus::Closed) break;
        if (st != host::RecvStatus::Ok || type != proto::MsgType::Control || !v.is_object()) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!got_hello_) {
                const json::Value* h = v.get("hello");
                const json::Value* e = v.get("error");
                hello_ = h && h->is_object() ? *h : e ? *e : json::Value();
                got_hello_ = true;
                hello_cv_.notify_all();
                continue;
            }
        }
        const json::Value* id = v.get("id");
        if (!id || !id->is_integer()) continue;
        std::shared_ptr<Pending> p;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(id->as_integer());
            if (it != pending_.end()) p = it->second;
        }
        if (!p) continue;
        std::lock_guard<std::mutex> lock(p->mutex);
        if (const json::Value* progress = v.get("progress")) {
            if (!p->closed && p->progress && progress->is_string()) p->progress(progress->as_string());
            continue;
        }
        p->reply = std::move(v);
        p->done = true;
        p->cv.notify_all();
    }
    alive_ = false;
    std::vector<std::shared_ptr<Pending>> waiting;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, p] : pending_) waiting.push_back(p);
        hello_cv_.notify_all();
    }
    for (auto& p : waiting) {
        std::lock_guard<std::mutex> lock(p->mutex);
        p->done = true; // with a null reply: the connection is gone
        p->cv.notify_all();
    }
}

json::Value HostClient::request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                                const std::function<void(const std::string&)>& progress,
                                const std::atomic<bool>* cancelled) {
    if (!alive_) throw HostError("lost the connection to the host at " + endpoint_);
    auto p = std::make_shared<Pending>();
    p->progress = progress;
    int64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
        pending_[id] = p;
    }
    auto forget = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
    };
    json::Value msg = json::Value::object();
    msg.set("id", id).set("method", method).set("params", std::move(params));
    bool sent;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        sent = host::send_json(*stream_, proto::MsgType::Control, msg);
    }
    if (!sent) {
        forget();
        throw HostError("lost the connection to the host at " + endpoint_);
    }
    const auto deadline = Clock::now() + timeout;
    bool done;
    json::Value reply;
    {
        std::unique_lock<std::mutex> lock(p->mutex);
        while (!p->done && !(cancelled && *cancelled) && Clock::now() < deadline) p->cv.wait_for(lock, 100ms);
        p->closed = true;
        done = p->done;
        reply = std::move(p->reply);
    }
    forget();
    if (!done)
        throw HostError(cancelled && *cancelled ? "cancelled" : "the host didn't answer within " +
                                                                    std::to_string(timeout.count()) + " ms");
    if (reply.is_null()) throw HostError("lost the connection to the host at " + endpoint_);
    if (const json::Value* e = reply.get("error")) {
        const json::Value* m = e->get("message");
        throw HostError(m && m->is_string() ? m->as_string() : "the host reported an error");
    }
    const json::Value* r = reply.get("result");
    return r ? *r : json::Value::object();
}

// ---- Launched ----------------------------------------------------------------

std::string Launched::host_output() const {
    std::lock_guard<std::mutex> lock(mutex);
    return output;
}

bool Launched::exited() const {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
}

int Launched::exit_status() const {
    std::lock_guard<std::mutex> lock(mutex);
    return status;
}

// ---- Hosts -------------------------------------------------------------------

Hosts::Hosts(HostsOptions options) : options_(std::move(options)) {}

Hosts::~Hosts() { stop_all(6s); }

std::vector<std::string> Hosts::candidate_endpoints() {
    std::set<std::string> found;
    for (const std::string& ep : host::list_endpoints(endpoint_prefix())) found.insert(ep);
    for (const std::string& ep : options_.endpoints) found.insert(ep);
    if (const char* env = std::getenv("HOTPP_ENDPOINT"); env && *env) found.insert(env);
    return {found.begin(), found.end()};
}

std::shared_ptr<Launched> Hosts::launched_at(const std::string& endpoint) {
    for (const auto& l : launched_)
        if (l->endpoint == endpoint) return l;
    return nullptr;
}

namespace {
// Identifies an endpoint, so a host that was incompatible isn't probed again until
// it is replaced.
std::string identity(const std::string& endpoint) { return host::endpoint_identity(endpoint); }
} // namespace

std::vector<Hosts::Entry> Hosts::discover() {
    std::vector<std::string> endpoints = candidate_endpoints();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& l : launched_)
        if (std::find(endpoints.begin(), endpoints.end(), l->endpoint) == endpoints.end() && !l->exited())
            endpoints.push_back(l->endpoint);
    std::vector<Entry> out;
    std::set<std::string> live;
    for (const std::string& ep : endpoints) {
        Entry e;
        e.endpoint = ep;
        e.launched = launched_at(ep);
        const bool explicit_endpoint =
            e.launched || std::find(options_.endpoints.begin(), options_.endpoints.end(), ep) != options_.endpoints.end();
        auto it = clients_.find(ep);
        if (it != clients_.end() && it->second->alive()) {
            e.client = it->second;
        } else {
            if (it != clients_.end()) clients_.erase(it);
            const std::string id = identity(ep);
            auto inc = incompatible_.find(ep);
            if (inc != incompatible_.end() && inc->second == id) {
                e.incompatible = true;
                e.error = "it doesn't accept controllers (a HotPP host older than 1.1?)";
            } else {
                std::string error;
                bool incompatible = false;
                e.client = HostClient::connect(ep, 2s, error, incompatible);
                if (e.client) {
                    clients_[ep] = e.client;
                } else if (incompatible) {
                    incompatible_[ep] = id;
                    e.incompatible = true;
                    e.error = error;
                } else if (!explicit_endpoint) {
                    continue; // a socket left behind by a host that's gone
                } else {
                    e.error = error;
                }
            }
        }
        live.insert(ep);
        out.push_back(std::move(e));
    }
    for (auto it = clients_.begin(); it != clients_.end();)
        it = live.count(it->first) ? std::next(it) : clients_.erase(it);
    return out;
}

std::vector<Hosts::Program> Hosts::programs(std::vector<Entry>* hosts) {
    std::vector<Entry> entries = discover();
    std::vector<Program> out;
    for (Entry& e : entries) {
        if (!e.client) continue;
        json::Value status;
        try {
            status = e.client->request("status", json::Value::object(), 5s);
        } catch (const HostError& err) {
            e.error = err.what();
            continue;
        }
        const json::Value* host = status.get("host");
        const json::Value* programs = status.get("programs");
        if (programs && programs->is_array())
            for (const json::Value& p : programs->as_array()) {
                Program prog;
                prog.host = e.client;
                prog.launched = e.launched;
                const json::Value* pid = p.get("pid");
                prog.pid = pid && pid->is_integer() ? uint32_t(pid->as_integer()) : 0;
                prog.info = p;
                if (host) prog.host_info = *host;
                out.push_back(std::move(prog));
            }
        // A launched program that exited: its host stays until it's stopped.
        if (e.launched && host)
            if (const json::Value* child = host->get("child"); child && child->get("exit_code")) {
                Program prog;
                prog.host = e.client;
                prog.launched = e.launched;
                prog.exited = true;
                const json::Value* pid = child->get("pid");
                prog.pid = pid && pid->is_integer() ? uint32_t(pid->as_integer()) : 0;
                prog.host_info = *host;
                out.push_back(std::move(prog));
            }
    }
    if (hosts) *hosts = std::move(entries);
    return out;
}

namespace {
std::string short_name(const Hosts::Program& p) {
    if (p.info.is_object())
        if (const json::Value* exe = p.info.get("exe"); exe && exe->is_string())
            return fs::path(exe->as_string()).filename().string();
    if (p.launched && !p.launched->command.empty()) return fs::path(p.launched->command[0]).filename().string();
    return "?";
}

std::string list_programs(const std::vector<Hosts::Program>& all) {
    std::string s;
    for (const auto& p : all)
        s += (s.empty() ? "" : ", ") + std::string("pid ") + std::to_string(p.pid) + " (" + short_name(p) +
             (p.exited ? ", exited" : "") + (p.launched ? ", launched here" : "") + ")";
    return s;
}
} // namespace

Hosts::Program Hosts::find(std::optional<uint32_t> pid, bool include_exited) {
    std::vector<Program> all = programs();
    if (!include_exited)
        all.erase(std::remove_if(all.begin(), all.end(), [](const Program& p) { return p.exited; }), all.end());
    if (pid) {
        const Program* best = nullptr;
        for (const Program& p : all)
            if (p.pid == *pid && (!best || (best->exited && !p.exited))) best = &p;
        if (best) return *best;
        throw HostError("No program with pid " + std::to_string(*pid) + " is connected to a HotPP host." +
                        (all.empty() ? std::string() : " Connected: " + list_programs(all) + "."));
    }
    if (all.size() == 1) return all[0];
    if (all.empty())
        throw HostError("No program is connected to a HotPP host. Start one with hotpp_launch, or run it under "
                        "the host (hotpp -- <program>) and try again.");
    std::vector<const Program*> mine;
    for (const Program& p : all)
        if (p.launched && !p.exited) mine.push_back(&p);
    if (mine.size() == 1) return *mine[0];
    throw HostError("Several programs are connected: " + list_programs(all) + ". Pass pid to choose one.");
}

std::shared_ptr<Launched> Hosts::launch(const LaunchSpec& spec, std::shared_ptr<HostClient>& client,
                                        std::chrono::milliseconds timeout) {
    if (!options_.allow_launch) throw HostError("launching programs is turned off (hotpp mcp --no-launch)");
    if (spec.command.empty() || spec.command[0].empty()) throw HostError("'command' needs at least the program to run");

    const std::string self = host::self_executable().string();
    const std::string endpoint = launch_endpoint(host::current_pid(), next_launch_++);

    std::string error;
    host::Pipe life, out;
    if (!host::make_pipe(life, error)) throw HostError(error);
    if (!host::make_pipe(out, error)) {
        host::close_handle(life.read);
        host::close_handle(life.write);
        throw HostError(error);
    }
    const host::Handle life_r = life.read, life_w = life.write, out_r = out.read, out_w = out.write;

    std::vector<std::string> argv = {self, "--endpoint", endpoint, "--capture-output", "--lifeline",
                                     host::handle_argument(life_r, 3)};
    if (!spec.auto_reload) argv.push_back("--no-auto-reload");
    if (!spec.build_command.empty()) argv.insert(argv.end(), {"--build", spec.build_command});
    if (spec.auto_restart) argv.push_back("--auto-restart");
    argv.push_back("--");
    argv.insert(argv.end(), spec.command.begin(), spec.command.end());

    host::SpawnOptions opt;
    opt.program = self;
    opt.cwd = spec.cwd;
    opt.null_stdin = true;
    opt.handles = {{out_w, 1}, {out_w, 2}, {life_r, 3}};
    opt.env = spec.env;
    opt.new_process_group = true; // Ctrl+C in the agent's terminal doesn't reach it; the lifeline ends it
    const int pid = host::spawn_process(argv, opt, error);
    host::close_handle(life_r);
    host::close_handle(out_w);
    if (pid < 0) {
        host::close_handle(life_w);
        host::close_handle(out_r);
        throw HostError(error);
    }

    auto l = std::make_shared<Launched>();
    l->endpoint = endpoint;
    l->host_pid = pid;
    l->command = spec.command;
    l->cwd = spec.cwd;
    l->lifeline = life_w;
    Launched* raw = l.get();
    l->reader = std::thread([raw, out_r] {
        constexpr size_t kKeep = 64 * 1024;
        char buf[4096];
        for (;;) {
            const long got = host::read_some(out_r, buf, sizeof buf);
            if (got <= 0) break;
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->output.append(buf, size_t(got));
            if (raw->output.size() > kKeep + kKeep / 4) raw->output.erase(0, raw->output.size() - kKeep);
        }
        host::close_handle(out_r);
    });
    l->reaper = std::thread([raw, pid] {
        const int status = host::wait_child(pid, {});
        host::release_child(pid);
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->status = status;
        raw->done = true;
        raw->cv.notify_all();
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        launched_.push_back(l);
    }

    // The host listens within milliseconds; give it a few seconds on a slow machine.
    const auto deadline = Clock::now() + std::min(timeout, std::chrono::milliseconds(15000));
    for (;;) {
        if (l->exited()) {
            const std::string out_text = l->host_output();
            const int status = l->exit_status();
            forget(l);
            throw HostError("the HotPP host exited (code " + std::to_string(status) + ") before it was ready:\n" +
                            out_text);
        }
        bool incompatible = false;
        if (auto c = HostClient::connect(endpoint, 2s, error, incompatible)) {
            client = c;
            std::lock_guard<std::mutex> lock(mutex_);
            clients_[endpoint] = c;
            return l;
        }
        if (Clock::now() >= deadline) {
            forget(l);
            throw HostError("the HotPP host didn't come up (" + error + ")");
        }
        std::this_thread::sleep_for(50ms);
    }
}

void Hosts::forget(const std::shared_ptr<Launched>& l) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        launched_.erase(std::remove(launched_.begin(), launched_.end(), l), launched_.end());
        clients_.erase(l->endpoint);
    }
    std::unique_lock<std::mutex> lock(l->mutex);
    if (l->lifeline != host::kNoHandle) {
        host::close_handle(l->lifeline); // the host stops its program and exits
        l->lifeline = host::kNoHandle;
    }
    if (!l->cv.wait_for(lock, 7s, [&] { return l->done; })) host::kill_process_group(l->host_pid);
    lock.unlock();
    if (l->reaper.joinable()) l->reaper.join();
    if (l->reader.joinable()) l->reader.join();
}

void Hosts::stop_all(std::chrono::milliseconds grace) {
    std::vector<std::shared_ptr<Launched>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all.swap(launched_);
        clients_.clear();
    }
    for (const auto& l : all) {
        std::lock_guard<std::mutex> lock(l->mutex);
        if (l->lifeline != host::kNoHandle) {
            host::close_handle(l->lifeline); // the host stops its program and exits
            l->lifeline = host::kNoHandle;
        }
    }
    const auto deadline = Clock::now() + grace;
    for (const auto& l : all) {
        std::unique_lock<std::mutex> lock(l->mutex);
        if (!l->cv.wait_until(lock, deadline, [&] { return l->done; })) host::kill_process_group(l->host_pid);
    }
    for (const auto& l : all) {
        if (l->reaper.joinable()) l->reaper.join();
        if (l->reader.joinable()) l->reader.join();
    }
}

uint64_t Hosts::seen(const std::string& endpoint, int64_t host_pid, uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = seen_.find(endpoint + "|" + std::to_string(host_pid) + "|" + std::to_string(pid));
    return it == seen_.end() ? 0 : it->second;
}

void Hosts::set_seen(const std::string& endpoint, int64_t host_pid, uint32_t pid, uint64_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t& s = seen_[endpoint + "|" + std::to_string(host_pid) + "|" + std::to_string(pid)];
    s = std::max(s, seq);
}

} // namespace hotpp::mcp
