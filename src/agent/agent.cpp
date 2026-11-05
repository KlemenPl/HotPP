// HotPP agent: lives inside the target process, reserves patch memory, talks to the
// host and applies the patches it sends.
#include "hotpp/hotpp.h"

#include "agent/patcher.h"
#include "agent/platform.h"
#include "common/endpoint.h"
#include "common/messages.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

namespace hotpp {

namespace agent {
void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[hotpp] %s\n", buf);
    std::fflush(stderr);
}
} // namespace agent

namespace {

using namespace hotpp::proto;

constexpr uint64_t kMinRegionSize = uint64_t(64) << 20;

struct AgentState {
    std::mutex send_mutex;
    agent::Connection* conn = nullptr;
    agent::Thread* reader = nullptr;
    std::mutex region_mutex;
    std::vector<RegionRecord> regions; // kept until process exit: patch code lives here
    std::vector<ModuleRecord> modules; // as reported in the hello
    std::atomic<bool> connected{false};
    bool initialized = false;

    bool async = false; // apply patches on the agent thread as they arrive
    std::string endpoint;
    Hello hello;                   // resent, marked resumed, after reconnecting
    std::atomic<bool> stopping{false}; // shutdown() or a hot restart: don't reconnect
    std::string exe;               // executable path at startup: a rebuild replaces this file
    std::vector<std::string> argv; // command line, for hot restart
    std::atomic<bool> restart_pending{false};

    std::mutex queue_mutex;
    std::vector<Patch> pending; // received, waiting for sync_point()
    std::atomic<bool> has_pending{false};
};

AgentState& state() {
    static AgentState s;
    return s;
}

// Hooks register during static initialization, possibly before init().
struct HookRegistry {
    std::mutex mutex;
    std::vector<void (*)()> pre, post, restart;
};

std::vector<void (*)()>& hook_list(HookRegistry& h, detail::HookKind kind) {
    switch (kind) {
    case detail::HookKind::PrePatch: return h.pre;
    case detail::HookKind::PostPatch: return h.post;
    default: return h.restart;
    }
}

HookRegistry& hooks() {
    static HookRegistry h;
    return h;
}

void run_hooks(detail::HookKind kind) {
    std::vector<void (*)()> fns;
    {
        HookRegistry& h = hooks();
        std::lock_guard<std::mutex> lock(h.mutex);
        fns = hook_list(h, kind);
    }
    for (void (*fn)() : fns) {
        try {
            fn();
        } catch (...) {
            agent::log("a hook threw; ignored");
        }
    }
}

bool send_message(AgentState& s, MsgType type, const Writer& w) {
    if (!w.ok()) return false;
    uint8_t header[kHeaderSize];
    encode_header(header, type, uint32_t(w.size()));
    std::lock_guard<std::mutex> lock(s.send_mutex);
    if (!s.conn) return false;
    return agent::ipc_send(s.conn, header, sizeof header) &&
           (w.size() == 0 || agent::ipc_send(s.conn, w.data(), w.size()));
}

bool recv_message(AgentState& s, Header& h, std::vector<uint8_t>& payload) {
    uint8_t header[kHeaderSize];
    if (!agent::ipc_recv(s.conn, header, sizeof header)) return false;
    if (!decode_header(header, h)) {
        agent::log("bad message header from host");
        return false;
    }
    if (h.version != kVersion) {
        agent::log("host speaks protocol version %u, agent speaks %u", unsigned(h.version),
                   unsigned(kVersion));
        return false;
    }
    payload.resize(h.size);
    return h.size == 0 || agent::ipc_recv(s.conn, payload.data(), h.size);
}

bool reserve_regions(uint64_t requested, const ModuleRecord& module, std::vector<RegionRecord>& out);

// Hot restart: the host rebuilt the program. Re-exec the executable (the new file at
// the same path) with the same arguments; the new process reconnects. Same pid.
void restart_now() {
    AgentState& s = state();
    run_hooks(detail::HookKind::PreRestart);
    agent::log("restarting %s", s.exe.c_str());
    const char* count = std::getenv("HOTPP_RESTARTED");
    setenv("HOTPP_RESTARTED", std::to_string((count ? std::atoi(count) : 0) + 1).c_str(), 1);
    std::fflush(nullptr);
    s.stopping = true;
    {
        std::lock_guard<std::mutex> lock(s.send_mutex);
        if (s.conn) agent::ipc_shutdown(s.conn); // the host sees the old session end
    }
    const std::string error = agent::exec_self(s.exe, s.argv);
    agent::log("hot restart failed: %s; the program keeps running without hot reload", error.c_str());
}

void apply_and_report(const Patch& patch) {
    AgentState& s = state();
    run_hooks(detail::HookKind::PrePatch);
    PatchResult result;
    result.id = patch.id;
    std::vector<RegionRecord> regions;
    {
        std::lock_guard<std::mutex> lock(s.region_mutex);
        regions = s.regions;
    }
    result.error = agent::apply_patch(patch, regions, s.modules);
    result.ok = result.error.empty();
    {
        // How much of each region is taken, so a host that reconnects later doesn't
        // place new code over it. Counted even if applying failed half way.
        std::lock_guard<std::mutex> lock(s.region_mutex);
        for (const Segment& seg : patch.segments)
            for (RegionRecord& r : s.regions)
                if (seg.addr >= r.base && seg.addr + seg.size <= r.base + r.size)
                    r.used = std::max(r.used, seg.addr + seg.size - r.base);
    }
    if (result.ok) run_hooks(detail::HookKind::PostPatch);
    else agent::log("patch %u failed: %s", unsigned(patch.id), result.error.c_str());
    Writer w;
    encode(w, result);
    if (s.connected) send_message(s, MsgType::PatchResult, w);
}

bool reconnect();

// Serves one connection until it ends.
void serve_connection() {
    AgentState& s = state();
    Header h;
    std::vector<uint8_t> payload;
    while (recv_message(s, h, payload)) {
        if (h.type == MsgType::Bye) break;
        Reader r(payload.data(), payload.size());
        if (h.type == MsgType::Log) {
            LogMsg msg;
            if (decode(r, msg)) agent::log("host: %s", msg.text.c_str());
        } else if (h.type == MsgType::Patch) {
            Patch patch;
            if (!decode(r, patch)) {
                agent::log("malformed patch from host; ignored");
                continue;
            }
            if (s.async) {
                apply_and_report(patch);
                continue;
            }
            std::lock_guard<std::mutex> lock(s.queue_mutex);
            s.pending.push_back(std::move(patch));
            s.has_pending = true;
        } else if (h.type == MsgType::Restart) {
            if (s.async) {
                restart_now();
                break; // only reached if exec failed; the connection is closed
            }
            s.restart_pending = true;
            s.has_pending = true;
        } else if (h.type == MsgType::ReserveRequest) {
            ReserveRequest req;
            if (!decode(r, req)) continue;
            ReserveReply reply;
            reply.id = req.id;
            if (req.module_index < s.modules.size()) {
                std::vector<RegionRecord> got;
                if (reserve_regions(req.size, s.modules[req.module_index], got)) {
                    reply.ok = true;
                    reply.region = got[0];
                    reply.region.module_index = req.module_index;
                    std::lock_guard<std::mutex> lock(s.region_mutex);
                    s.regions.push_back(reply.region);
                }
            }
            Writer w;
            encode(w, reply);
            send_message(s, MsgType::ReserveReply, w);
        } else if (h.type == MsgType::ResolveRequest) {
            ResolveRequest req;
            if (!decode(r, req)) continue;
            ResolveReply reply;
            reply.id = req.id;
            for (const std::string& name : req.names)
                reply.addresses.push_back(agent::resolve_symbol(name));
            Writer w;
            encode(w, reply);
            send_message(s, MsgType::ResolveReply, w);
        }
        // Unknown message types are skipped, so newer hosts can add messages.
    }
}

void reader_main(void*) {
    agent::mark_agent_thread();
    AgentState& s = state();
    for (;;) {
        serve_connection();
        s.connected = false;
        if (s.stopping || !reconnect()) return;
    }
}

// The host went away (crashed, or was restarted): keep trying to reach one on the
// same endpoint, and tell it which patches are already in place.
bool reconnect() {
    AgentState& s = state();
    agent::log("lost the host; reconnecting to %s", s.endpoint.c_str());
    unsigned delay = 250;
    while (!s.stopping) {
        for (unsigned waited = 0; waited < delay && !s.stopping; waited += 50) agent::sleep_ms(50);
        if (s.stopping) break;
        agent::Connection* c = agent::ipc_connect(s.endpoint);
        delay = std::min(delay * 2, 5000u);
        if (!c) continue;
        agent::Connection* old;
        {
            std::lock_guard<std::mutex> lock(s.send_mutex);
            old = s.conn;
            s.conn = c;
        }
        if (old) agent::ipc_close(old);

        Hello hello = s.hello;
        hello.resumed = true;
        {
            std::lock_guard<std::mutex> lock(s.region_mutex);
            hello.regions = s.regions;
        }
        Writer w;
        encode(w, hello);
        Header h;
        std::vector<uint8_t> payload;
        if (send_message(s, MsgType::Hello, w) && recv_message(s, h, payload) && h.type == MsgType::HelloAck) {
            s.connected = true;
            agent::log("reconnected to the host on %s", s.endpoint.c_str());
            return true;
        }
    }
    return false;
}

// Config value, then environment variable, then fallback.
std::string pick(const char* configured, const char* env_name, const std::string& fallback) {
    if (configured && *configured) return configured;
    std::string env = agent::get_env(env_name);
    return env.empty() ? fallback : env;
}

// Without a configured path (no hotpp_enable(), e.g. with the single header): the
// executable's directory and its parents (a CMake build tree keeps the file at its
// top), then the working directory.
std::string find_compile_commands(const std::string& exe) {
    auto readable = [](const std::string& p) { return access(p.c_str(), R_OK) == 0; };
    std::string dir = exe.substr(0, exe.rfind('/'));
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const std::string candidate = dir + "/compile_commands.json";
        if (readable(candidate)) return candidate;
        const size_t slash = dir.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        dir.resize(slash);
    }
    char cwd[PATH_MAX];
    if (readable("compile_commands.json") && getcwd(cwd, sizeof cwd)) return std::string(cwd) + "/compile_commands.json";
    return {};
}

agent::Connection* connect_or_launch(const std::string& endpoint, const Config& cfg) {
    if (agent::Connection* c = agent::ipc_connect(endpoint)) return c;
    if (!cfg.launch_host || agent::get_env("HOTPP_LAUNCH_HOST") == "0") return nullptr;

    std::string host = agent::get_env("HOTPP_HOST");
    if (host.empty()) host = cfg.host_path && *cfg.host_path ? cfg.host_path : "hotpp";
    agent::log("no host listening on %s, launching '%s'", endpoint.c_str(), host.c_str());
    if (!agent::spawn_detached(host, {"--endpoint", endpoint})) {
        agent::log("could not launch '%s'", host.c_str());
        return nullptr;
    }
    for (int i = 0; i < 60; ++i) {
        agent::sleep_ms(50);
        if (agent::Connection* c = agent::ipc_connect(endpoint)) return c;
    }
    return nullptr;
}

// A region next to `module`. If the requested size doesn't fit within reach,
// smaller sizes are tried.
bool reserve_regions(uint64_t requested, const ModuleRecord& module, std::vector<RegionRecord>& out) {
    for (uint64_t size = requested; size >= kMinRegionSize; size /= 2) {
        RegionRecord region;
        if (agent::reserve_near(module, size, region)) {
            out.push_back(region);
            return true;
        }
    }
    return false;
}

void release_all(AgentState& s) {
    for (const RegionRecord& r : s.regions) agent::release_region(r);
    s.regions.clear();
}

} // namespace

bool init(const Config& cfg) {
    AgentState& s = state();
    if (s.initialized) return s.connected;
    s.initialized = true;

    if (agent::get_env("HOTPP_DISABLE") == "1") return false;
    s.async = cfg.async || agent::get_env("HOTPP_ASYNC") == "1";

    Hello hello;
    hello.pid = agent::current_pid();
    hello.platform = Platform::Linux;
#if defined(__x86_64__)
    hello.arch = Arch::X64;
#endif
    hello.exe = agent::executable_path();
    s.exe = hello.exe;
    s.argv = agent::command_line();
    hello.compile_commands = pick(nullptr, "HOTPP_COMPILE_COMMANDS",
                                  cfg.compile_commands ? cfg.compile_commands : "");
    if (hello.compile_commands.empty()) hello.compile_commands = find_compile_commands(hello.exe);
    hello.modules = agent::enumerate_modules();
    if (hello.modules.empty()) {
        agent::log("could not enumerate loaded modules; hot reload disabled");
        return false;
    }
    // The executable's region up front; the host asks for regions next to the
    // shared libraries it patches.
    if (!reserve_regions(cfg.region_size, hello.modules[0], s.regions)) {
        agent::log("could not reserve patch memory within 2 GiB of %s; hot reload disabled",
                   hello.exe.c_str());
        return false;
    }
    hello.regions = s.regions;
    s.modules = hello.modules;
    s.hello = hello;

    const std::string endpoint = pick(cfg.endpoint, "HOTPP_ENDPOINT", default_endpoint());
    s.endpoint = endpoint;
    s.conn = connect_or_launch(endpoint, cfg);
    if (!s.conn) {
        agent::log("no host on %s; running without hot reload", endpoint.c_str());
        release_all(s);
        return false;
    }

    Writer w;
    encode(w, hello);
    Header h;
    std::vector<uint8_t> payload;
    HelloAck ack;
    bool ok = send_message(s, MsgType::Hello, w) && recv_message(s, h, payload) &&
              h.type == MsgType::HelloAck;
    if (ok) {
        Reader r(payload.data(), payload.size());
        ok = decode(r, ack);
    }
    if (!ok) {
        agent::log("handshake with host on %s failed; running without hot reload",
                   endpoint.c_str());
        agent::ipc_close(s.conn);
        s.conn = nullptr;
        release_all(s);
        return false;
    }

    s.connected = true;
    s.reader = agent::thread_start(reader_main, nullptr);
    if (!s.reader) {
        agent::log("could not start the agent thread; running without hot reload");
        s.connected = false;
        agent::ipc_close(s.conn);
        s.conn = nullptr;
        release_all(s);
        return false;
    }
    agent::log("connected to hotpp %s (pid %u) on %s%s", ack.host_version.c_str(), unsigned(ack.host_pid),
               endpoint.c_str(), s.async ? ", async mode" : "");
    return true;
}

void sync_point() {
    AgentState& s = state();
    if (!s.has_pending.load(std::memory_order_acquire)) return;
    std::vector<Patch> todo;
    {
        std::lock_guard<std::mutex> lock(s.queue_mutex);
        todo.swap(s.pending);
        s.has_pending = false;
    }
    for (const Patch& patch : todo) apply_and_report(patch);
    if (s.restart_pending.exchange(false)) restart_now();
}

namespace detail {
void register_hook(HookKind kind, void (*fn)()) {
    if (!fn) return;
    HookRegistry& h = hooks();
    std::lock_guard<std::mutex> lock(h.mutex);
    auto& list = hook_list(h, kind);
    // A patch can re-run a file's initializers (docs/DESIGN.md §8.6); register once.
    if (std::find(list.begin(), list.end(), fn) == list.end()) list.push_back(fn);
}
} // namespace detail

void shutdown() {
    AgentState& s = state();
    if (!s.reader) return;
    s.stopping = true;
    Writer bye;
    send_message(s, MsgType::Bye, bye);
    {
        std::lock_guard<std::mutex> lock(s.send_mutex);
        if (s.conn) agent::ipc_shutdown(s.conn);
    }
    if (s.reader) {
        agent::thread_join(s.reader);
        s.reader = nullptr;
    }
    if (s.conn) agent::ipc_close(s.conn);
    s.conn = nullptr;
    s.connected = false;
}

bool connected() { return state().connected; }

bool restarted() {
    const char* count = std::getenv("HOTPP_RESTARTED");
    return count && std::atoi(count) > 0;
}

} // namespace hotpp
