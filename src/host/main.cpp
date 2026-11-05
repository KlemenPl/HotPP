// hotpp: the HotPP host. Agents in running programs connect to it; from M1 on it
// watches sources, recompiles and sends patches.
//
//   hotpp [--endpoint <name>] [--once]      serve: wait for agents
//   hotpp [--endpoint <name>] -- <program>   run: start <program> connected to this host
#include "common/endpoint.h"
#include "common/messages.h"
#include "host/platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hotpp;
using namespace hotpp::proto;

constexpr const char* kUsage =
    "usage: hotpp [--endpoint <name>] [--once]\n"
    "       hotpp [--endpoint <name>] -- <program> [args...]\n"
    "\n"
    "Without a program, waits for agents to connect (--once: exit after the first\n"
    "one disconnects). With a program, starts it connected to this host and exits\n"
    "with its exit code; exits 2 if no agent connected, 3 if an agent reported an\n"
    "unusable memory layout.\n";

struct Options {
    std::string endpoint;
    bool once = false;
    std::vector<std::string> program;
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--") == 0) {
            o.program.assign(argv + i + 1, argv + argc);
            return !o.program.empty();
        }
        if (std::strcmp(a, "--endpoint") == 0 && i + 1 < argc) {
            o.endpoint = argv[++i];
        } else if (std::strcmp(a, "--once") == 0) {
            o.once = true;
        } else {
            return false;
        }
    }
    return true;
}

struct Server {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0;   // connections being handled
    int sessions_ended = 0; // agents that sent a hello and then disconnected
    int problems = 0;       // agents with an unusable layout
};

bool send_message(host::Stream& s, MsgType type, const Writer& w) {
    uint8_t header[kHeaderSize];
    encode_header(header, type, uint32_t(w.size()));
    return w.ok() && s.send(header, sizeof header) && (w.size() == 0 || s.send(w.data(), w.size()));
}

bool recv_message(host::Stream& s, Header& h, std::vector<uint8_t>& payload) {
    uint8_t header[kHeaderSize];
    if (!s.recv(header, sizeof header)) return false;
    if (!decode_header(header, h)) {
        std::fprintf(stderr, "[hotpp] bad message header from agent\n");
        return false;
    }
    if (h.version != kVersion) {
        std::fprintf(stderr, "[hotpp] agent speaks protocol version %u, host speaks %u\n",
                     unsigned(h.version), unsigned(kVersion));
        return false;
    }
    payload.resize(h.size);
    return h.size == 0 || s.recv(payload.data(), h.size);
}

std::string format_size(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

const char* platform_name(Platform p) {
    switch (p) {
    case Platform::Linux: return "linux";
    case Platform::Windows: return "windows";
    default: return "unknown-os";
    }
}

const char* arch_name(Arch a) { return a == Arch::X64 ? "x64" : "unknown-arch"; }

uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Prints what the agent reported. Returns false if it can't be patched: no region,
// or a region outside rel32 reach of its module.
bool print_hello(const Hello& h) {
    std::string out;
    char line[1024];
    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
    add("[hotpp] agent connected: pid %u (%s %s)\n", unsigned(h.pid), platform_name(h.platform),
        arch_name(h.arch));
    add("  exe:              %s\n", h.exe.c_str());
    add("  compile commands: %s\n", h.compile_commands.empty() ? "(not set)" : h.compile_commands.c_str());
    add("  modules (%zu):\n", h.modules.size());
    for (size_t i = 0; i < h.modules.size(); ++i) {
        const ModuleRecord& m = h.modules[i];
        add("    [%2zu] %016llx-%016llx %10s  %s\n", i, (unsigned long long)m.start,
            (unsigned long long)(m.start + m.size), format_size(m.size).c_str(),
            m.path.empty() ? "(unnamed)" : m.path.c_str());
    }
    bool ok = !h.regions.empty();
    add("  patch regions (%zu):\n", h.regions.size());
    for (const RegionRecord& r : h.regions) {
        if (r.module_index >= h.modules.size()) {
            add("    %016llx  invalid module index %u\n", (unsigned long long)r.base,
                unsigned(r.module_index));
            ok = false;
            continue;
        }
        const ModuleRecord& m = h.modules[r.module_index];
        const uint64_t farthest =
            std::max(distance(r.base, m.start + m.size), distance(r.base + r.size, m.start));
        const bool in_reach = farthest < (uint64_t(1) << 31);
        ok = ok && in_reach;
        add("    %016llx-%016llx %10s  near [%u], farthest byte %s away%s\n",
            (unsigned long long)r.base, (unsigned long long)(r.base + r.size),
            format_size(r.size).c_str(), unsigned(r.module_index), format_size(farthest).c_str(),
            in_reach ? "" : "  OUT OF RANGE");
    }
    if (h.regions.empty()) out += "  error: the agent reserved no patch region\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    return ok;
}

void finish(Server& server, bool hello, bool problem) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        --server.active;
        server.sessions_ended += hello;
        server.problems += problem;
    }
    server.cv.notify_all();
}

void handle_client(std::unique_ptr<host::Stream> stream, Server* server) {
    Header h;
    std::vector<uint8_t> payload;
    Hello hello;
    if (!recv_message(*stream, h, payload)) {
        // Closed without sending anything, e.g. another host probing the endpoint.
        finish(*server, false, false);
        return;
    }
    if (h.type != MsgType::Hello) {
        std::fprintf(stderr, "[hotpp] client sent message type %u before hello; dropped\n",
                     unsigned(h.type));
        finish(*server, false, false);
        return;
    }
    Reader r(payload.data(), payload.size());
    if (!decode(r, hello)) {
        std::fprintf(stderr, "[hotpp] malformed hello; dropped\n");
        finish(*server, false, false);
        return;
    }
    const bool layout_ok = print_hello(hello);

    HelloAck ack;
    ack.host_pid = host::current_pid();
    ack.host_version = HOTPP_VERSION;
    Writer w;
    encode(w, ack);
    if (send_message(*stream, MsgType::HelloAck, w)) {
        while (recv_message(*stream, h, payload)) {
            if (h.type == MsgType::Bye) break;
            if (h.type == MsgType::Log) {
                Reader lr(payload.data(), payload.size());
                LogMsg msg;
                if (decode(lr, msg)) std::printf("[agent %u] %s\n", unsigned(hello.pid), msg.text.c_str());
            }
        }
    }
    std::printf("[hotpp] agent disconnected: pid %u\n", unsigned(hello.pid));
    std::fflush(stdout);
    finish(*server, true, !layout_ok);
}

} // namespace

int main(int argc, char** argv) {
    host::init_console();
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fputs(kUsage, stderr);
        return 1;
    }
    const bool run_mode = !opt.program.empty();
    // Serve mode honours $HOTPP_ENDPOINT like the agent does. Run mode uses a private
    // endpoint and hands it to the child, so concurrent runs never collide.
    const char* env_endpoint = std::getenv("HOTPP_ENDPOINT");
    std::string endpoint = opt.endpoint;
    if (endpoint.empty())
        endpoint = run_mode                       ? unique_endpoint(host::current_pid())
                   : env_endpoint && *env_endpoint ? std::string(env_endpoint)
                                                  : default_endpoint();

    std::string error;
    // Both outlive main's scope: detached threads use them until the process exits.
    host::Listener* listener = host::listen(endpoint, error).release();
    if (!listener) {
        std::fprintf(stderr, "[hotpp] error: %s\n", error.c_str());
        return 1;
    }
    auto* server = new Server;
    std::printf("[hotpp] host %s listening on %s\n", HOTPP_VERSION, endpoint.c_str());
    std::fflush(stdout);

    std::thread([listener, server] {
        while (std::unique_ptr<host::Stream> s = listener->accept()) {
            {
                std::lock_guard<std::mutex> lock(server->mutex);
                ++server->active;
            }
            std::thread(handle_client, std::move(s), server).detach();
        }
    }).detach();

    if (run_mode) {
        host::set_env("HOTPP_ENDPOINT", endpoint);
        int code = host::run_child(opt.program, error);
        if (code < 0) {
            std::fprintf(stderr, "[hotpp] error: %s\n", error.c_str());
            listener->close();
            return 1;
        }
        std::unique_lock<std::mutex> lock(server->mutex);
        server->cv.wait_for(lock, std::chrono::seconds(2), [&] { return server->active == 0; });
        const int hellos = server->sessions_ended, problems = server->problems;
        lock.unlock();
        listener->close();
        std::printf("[hotpp] %s exited with code %d\n", opt.program[0].c_str(), code);
        if (code != 0) return code;
        if (hellos == 0) {
            std::fprintf(stderr, "[hotpp] error: no agent connected\n");
            return 2;
        }
        return problems ? 3 : 0;
    }

    std::unique_lock<std::mutex> lock(server->mutex);
    server->cv.wait(lock, [&] { return opt.once && server->sessions_ended > 0; });
    const int problems = server->problems;
    lock.unlock();
    listener->close();
    return problems ? 3 : 0;
}
