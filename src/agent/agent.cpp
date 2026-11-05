// HotPP agent: lives inside the target process, reserves patch memory, talks to the
// host and applies the patches it sends.
#include "hotpp/hotpp.h"

#include "agent/patcher.h"
#include "agent/platform.h"
#include "common/endpoint.h"
#include "common/messages.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

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
    std::vector<void (*)()> pre, post;
};

HookRegistry& hooks() {
    static HookRegistry h;
    return h;
}

void run_hooks(detail::HookKind kind) {
    std::vector<void (*)()> fns;
    {
        HookRegistry& h = hooks();
        std::lock_guard<std::mutex> lock(h.mutex);
        fns = kind == detail::HookKind::PrePatch ? h.pre : h.post;
    }
    for (void (*fn)() : fns) {
        try {
            fn();
        } catch (...) {
            agent::log("a %s-patch hook threw; ignored", kind == detail::HookKind::PrePatch ? "pre" : "post");
        }
    }
}

bool send_message(AgentState& s, MsgType type, const Writer& w) {
    if (!w.ok()) return false;
    uint8_t header[kHeaderSize];
    encode_header(header, type, uint32_t(w.size()));
    std::lock_guard<std::mutex> lock(s.send_mutex);
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
    if (result.ok) run_hooks(detail::HookKind::PostPatch);
    else agent::log("patch %u failed: %s", unsigned(patch.id), result.error.c_str());
    Writer w;
    encode(w, result);
    if (s.connected) send_message(s, MsgType::PatchResult, w);
}

void reader_main(void*) {
    agent::mark_agent_thread();
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
    s.connected = false;
}

// Config value, then environment variable, then fallback.
std::string pick(const char* configured, const char* env_name, const std::string& fallback) {
    if (configured && *configured) return configured;
    std::string env = agent::get_env(env_name);
    return env.empty() ? fallback : env;
}

agent::Connection* connect_or_launch(const std::string& endpoint, const Config& cfg) {
    if (agent::Connection* c = agent::ipc_connect(endpoint)) return c;
    if (!cfg.launch_host || agent::get_env("HOTPP_LAUNCH_HOST") == "0") return nullptr;

    const std::string host = pick(cfg.host_path, "HOTPP_HOST", "hotpp");
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
    hello.compile_commands = pick(nullptr, "HOTPP_COMPILE_COMMANDS",
                                  cfg.compile_commands ? cfg.compile_commands : "");
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

    const std::string endpoint = pick(cfg.endpoint, "HOTPP_ENDPOINT", default_endpoint());
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
}

namespace detail {
void register_hook(HookKind kind, void (*fn)()) {
    if (!fn) return;
    HookRegistry& h = hooks();
    std::lock_guard<std::mutex> lock(h.mutex);
    auto& list = kind == HookKind::PrePatch ? h.pre : h.post;
    // A patch can re-run a file's initializers (docs/DESIGN.md §8.6); register once.
    if (std::find(list.begin(), list.end(), fn) == list.end()) list.push_back(fn);
}
} // namespace detail

void shutdown() {
    AgentState& s = state();
    if (!s.conn) return;
    Writer bye;
    send_message(s, MsgType::Bye, bye);
    agent::ipc_shutdown(s.conn);
    if (s.reader) {
        agent::thread_join(s.reader);
        s.reader = nullptr;
    }
    agent::ipc_close(s.conn);
    s.conn = nullptr;
    s.connected = false;
}

bool connected() { return state().connected; }

} // namespace hotpp
