// hotpp: the HotPP host. Agents in running programs connect to it; it watches their
// sources, recompiles what changes and sends patches. Controllers (hotpp mcp)
// connect to the same socket to watch and steer it.
//
//   hotpp [options] [--once]          serve: wait for agents
//   hotpp [options] -- <program>      run: start <program> connected to this host
//   hotpp mcp [options]               MCP server for AI agents (src/mcp)
#include "common/endpoint.h"
#include "common/protocol.h"
#include "host/control.h"
#include "host/control_protocol.h"
#include "host/platform.h"
#include "host/session.h"
#include "mcp/mcp.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hotpp;

constexpr const char* kUsage =
    "usage: hotpp [options] [--once]\n"
    "       hotpp [options] -- <program> [args...]\n"
    "       hotpp mcp [mcp options]    (see hotpp mcp --help)\n"
    "       hotpp restart [--pid N]    rebuild and restart programs (see hotpp restart --help)\n"
    "\n"
    "Without a program, waits for agents to connect (--once: exit after the first\n"
    "one disconnects). With a program, starts it connected to this host and exits\n"
    "with its exit code; exits 2 if no agent connected, 3 if an agent reported an\n"
    "unusable memory layout. hotpp mcp serves AI agents over the Model Context\n"
    "Protocol.\n"
    "\n"
    "options:\n"
    "  --endpoint <path>   socket to listen on\n"
    "  --build <command>   rebuild command for hot restarts (run with the shell)\n"
    "  --auto-restart      rebuild and restart when a change can't be patched\n"
    "  --no-auto-reload    don't patch saved files until a controller asks (hotpp mcp)\n"
    "  --capture-output    run the program on a pseudo-terminal and keep its output\n"
    "                      for controllers (it is still printed)\n"
    "  --lifeline <handle> stop the program and exit once <handle> reads end of file\n"
    "                      (used by hotpp mcp)\n"
    "  -v, --verbose       full connect info and every binding decision\n"
    "  --version           print the version and exit\n";

struct Options {
    std::string endpoint;
    bool once = false;
    bool version = false;
    bool capture_output = false;
    host::Handle lifeline = host::kNoHandle;
    host::HostOptions host;
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
        } else if (std::strcmp(a, "--version") == 0) {
            o.version = true;
        } else if (std::strcmp(a, "--once") == 0) {
            o.once = true;
        } else if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--verbose") == 0) {
            o.host.verbose = true;
        } else if (std::strcmp(a, "--build") == 0 && i + 1 < argc) {
            o.host.build_command = argv[++i];
        } else if (std::strcmp(a, "--auto-restart") == 0) {
            o.host.auto_restart = true;
        } else if (std::strcmp(a, "--no-auto-reload") == 0) {
            o.host.auto_reload = false;
        } else if (std::strcmp(a, "--capture-output") == 0) {
            o.capture_output = true;
        } else if (std::strcmp(a, "--lifeline") == 0 && i + 1 < argc) {
            o.lifeline = host::parse_handle(argv[++i]);
            if (o.lifeline == host::kNoHandle) return false;
        } else {
            return false;
        }
    }
    return true;
}

// $HOTPP_TRACE_SHUTDOWN: print each step of shutting down, to find one that hangs.
void trace(const char* step) {
    static const bool on = std::getenv("HOTPP_TRACE_SHUTDOWN") != nullptr;
    if (!on) return;
    std::fprintf(stderr, "[hotpp] shutdown: %s\n", step);
    std::fflush(stderr);
}

// Set by Ctrl+C / SIGTERM: shut down cleanly (remove the endpoint, end sessions).
std::atomic<bool> g_stop{false};
std::atomic<bool> g_terminate{false}; // a Terminate request (not Ctrl+C): pass it on to the program
std::atomic<int> g_interrupts{0};

void on_stop(host::StopRequest r) {
    if (r == host::StopRequest::Interrupt) g_interrupts.fetch_add(1);
    else g_terminate = true;
    g_stop = true;
}

void on_restart() { host::g_restart_requests.fetch_add(1); }

constexpr uint32_t kMaxHello = 64u << 20;

struct Server {
    std::mutex mutex;
    std::condition_variable cv;
    int agents = 0;         // agent connections being handled
    int sessions_ended = 0; // agents that sent a hello and then disconnected
    int problems = 0;       // agents with an unusable layout
    std::vector<std::thread> threads;      // joined at shutdown
    std::map<host::Stream*, bool> open;    // live connections, ended at shutdown; true: a controller
    host::HostContext ctx;
};

void serve_controller_connection(host::Stream& stream, const proto::Header& h, Server* server) {
    json::Value hello;
    if (host::recv_json_payload(stream, h, hello, host::kMaxControlRequest) != host::RecvStatus::Ok) return;
    if (!stream.peer_is_same_user()) {
        std::fprintf(stderr, "[hotpp] refused a controller of another user (%s)\n", stream.peer_description().c_str());
        return;
    }
    if (server->ctx.controllers.fetch_add(1) >= host::kMaxControllers) {
        --server->ctx.controllers;
        json::Value e = json::Value::object();
        e.set("message", "too many controllers are connected to this host");
        json::Value r = json::Value::object();
        r.set("error", std::move(e));
        host::send_json(stream, proto::MsgType::Control, r);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->open[&stream] = true;
    }
    host::serve_controller(stream, hello, server->ctx);
    --server->ctx.controllers;
}

void handle_client(std::unique_ptr<host::Stream> stream, Server* server) {
    host::Stream* raw = stream.get();
    auto forget = [&] {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->open.erase(raw); // before the stream goes away
    };
    uint8_t header[proto::kHeaderSize];
    proto::Header h;
    if (!raw->recv(header, sizeof header)) return forget(); // closed without a word, e.g. another host probing
    if (!proto::decode_header(header, h)) {
        std::fprintf(stderr, "[hotpp] bad message header from a client; dropped\n");
        return forget();
    }
    if (h.type == proto::MsgType::ControlHello) {
        serve_controller_connection(*raw, h, server);
        return forget();
    }
    if (h.version != proto::kVersion) {
        std::fprintf(stderr, "[hotpp] agent speaks protocol version %u, host speaks %u\n", unsigned(h.version),
                     unsigned(proto::kVersion));
        return forget();
    }
    if (h.type != proto::MsgType::Hello || h.size > kMaxHello) {
        std::fprintf(stderr, "[hotpp] client sent message type %u before hello; dropped\n", unsigned(h.type));
        return forget();
    }
    std::vector<uint8_t> payload(h.size);
    if (h.size && !raw->recv(payload.data(), h.size)) return forget();
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        ++server->agents;
    }
    host::Session::Outcome outcome;
    {
        host::Session session(std::move(stream), server->ctx.options, server->ctx.events, server->ctx.registry);
        outcome = session.run(payload);
        forget(); // before the session (and its stream) goes away
    }
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        --server->agents;
        server->sessions_ended += outcome.hello;
        server->problems += outcome.hello && !outcome.layout_ok;
    }
    server->cv.notify_all();
}

// Stops accepting, ends every open connection and waits for all connection threads.
// Controllers only lose their receiving side, so replies in progress still go out.
void shut_down(host::Listener& listener, std::thread& acceptor, Server& server) {
    trace("closing the listener");
    listener.close();
    if (acceptor.joinable()) acceptor.join();
    trace("ending connections");
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        for (auto& [s, controller] : server.open) controller ? s->shutdown_read() : s->shutdown();
    }
    for (std::thread& t : server.threads) t.join();
    trace("connections ended");
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "mcp") == 0) return hotpp::mcp::run(argc - 1, argv + 1);
    if (argc >= 2 && std::strcmp(argv[1], "restart") == 0) return hotpp::mcp::run_restart(argc - 1, argv + 1);

    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fputs(kUsage, stderr);
        return 1;
    }
    if (opt.version) {
        std::printf("hotpp %s\n", HOTPP_VERSION);
        return 0;
    }
    const bool run_mode = !opt.program.empty();
    if (opt.capture_output && !run_mode) {
        std::fputs("[hotpp] error: --capture-output needs a program (hotpp --capture-output -- <program>)\n", stderr);
        return 1;
    }
    // Serve mode honours $HOTPP_ENDPOINT like the agent does. Run mode uses a private
    // endpoint and hands it to the child, so concurrent runs never collide.
    const char* env_endpoint = std::getenv("HOTPP_ENDPOINT");
    std::string endpoint = opt.endpoint;
    if (endpoint.empty())
        endpoint = run_mode                        ? unique_endpoint(host::current_pid())
                   : env_endpoint && *env_endpoint ? std::string(env_endpoint)
                                                   : default_endpoint();

    std::string error;
    std::unique_ptr<host::Listener> listener = host::listen(endpoint, error);
    if (!listener) {
        std::fprintf(stderr, "[hotpp] error: %s\n", error.c_str());
        return 1;
    }
    // Before any thread starts: setenv() isn't safe while other threads read the
    // environment (starting compilers does).
    if (run_mode) host::set_env("HOTPP_ENDPOINT", endpoint);
    Server server;
    host::HostContext& ctx = server.ctx;
    ctx.options = opt.host;
    ctx.endpoint = endpoint;
    ctx.run_mode = run_mode;
    ctx.lifeline = opt.lifeline != host::kNoHandle;
    if (opt.capture_output) ctx.output = std::make_unique<host::OutputBuffer>();
    {
        std::lock_guard<std::mutex> lock(ctx.child.mutex);
        ctx.child.command = opt.program;
    }

    // SIGTERM to the program, SIGKILL if it's still there after the grace period.
    std::mutex killer_mutex;
    std::thread killer;
    ctx.stop_child = [&](std::chrono::milliseconds grace) {
        {
            std::lock_guard<std::mutex> lock(ctx.child.mutex);
            if (!ctx.child.running) return;
            host::stop_process(ctx.child.pid, false); // not reaped yet (running), so the pid is still the program's
        }
        std::lock_guard<std::mutex> lock(killer_mutex);
        if (killer.joinable()) return;
        killer = std::thread([&ctx, grace] {
            std::unique_lock<std::mutex> lock(ctx.child.mutex);
            if (!ctx.child.cv.wait_for(lock, grace, [&] { return !ctx.child.running; }))
                host::stop_process(ctx.child.pid, true);
        });
    };

    host::install_signal_handlers(on_stop, on_restart);
    std::printf("[hotpp] host %s listening on %s\n", HOTPP_VERSION, endpoint.c_str());
    std::fflush(stdout);

    // Wakes the lifeline and output threads at shutdown.
    host::WakeEvent wake;
    std::atomic<bool> lifeline_lost{false};
    std::thread lifeline;
    if (opt.lifeline != host::kNoHandle) {
        host::keep_private(opt.lifeline); // not for the program
        lifeline = std::thread([&] {
            for (;;) {
                char c;
                const long n = host::read_some(opt.lifeline, &c, 1, -1, &wake);
                if (n > 0) continue;
                if (n == -2) return; // shutting down
                break;               // end of file (or an error): whoever held the other end is gone
            }
            std::printf("[hotpp] the controller that started this host is gone; stopping\n");
            std::fflush(stdout);
            lifeline_lost = true;
            g_terminate = true;
            g_stop = true;
            if (ctx.stop_child) ctx.stop_child(std::chrono::seconds(5));
            {
                std::lock_guard<std::mutex> lock(ctx.child.mutex);
            }
            ctx.child.cv.notify_all();
        });
    }

    std::thread acceptor([&] {
        while (std::unique_ptr<host::Stream> s = listener->accept()) {
            std::lock_guard<std::mutex> lock(server.mutex);
            server.open[s.get()] = false;
            server.threads.emplace_back(handle_client, std::move(s), &server);
        }
    });

    auto join_helpers = [&] {
        wake.set();
        if (lifeline.joinable()) lifeline.join();
        std::lock_guard<std::mutex> lock(killer_mutex);
        if (killer.joinable()) killer.join();
    };

    if (run_mode) {
        host::Handle master = host::kNoHandle;
        const int pid = opt.capture_output ? host::spawn_child_pty(opt.program, master, error)
                                           : host::spawn_child(opt.program, error);
        if (pid < 0) {
            std::fprintf(stderr, "[hotpp] error: %s\n", error.c_str());
            shut_down(*listener, acceptor, server);
            join_helpers();
            return 1;
        }
        {
            std::lock_guard<std::mutex> lock(ctx.child.mutex);
            ctx.child.pid = pid;
            ctx.child.running = true;
        }
        if (lifeline_lost) ctx.stop_child(std::chrono::seconds(5));
        // The program's output: kept for controllers and printed.
        std::thread output;
        if (master != host::kNoHandle)
            output = std::thread([&, master] {
                char buf[8192];
                bool stopping = false;
                for (;;) {
                    // Once shutting down, read only what is already there.
                    const long n = host::read_some(master, buf, sizeof buf, stopping ? 0 : -1, stopping ? nullptr : &wake);
                    if (n == -2) {
                        if (stopping) break;
                        stopping = true;
                        continue;
                    }
                    if (n <= 0) break; // every writer is gone
                    ctx.output->append(std::string_view(buf, size_t(n)));
                    std::fwrite(buf, 1, size_t(n), stdout);
                    std::fflush(stdout);
                }
                ctx.output->close();
                host::close_handle(master);
            });

        // Ctrl+C reaches the child too (same process group); a stop request sent to
        // the host alone, or a second Ctrl+C, is passed on.
        int current = pid;
        auto forward = [&current] {
            if (g_terminate) host::stop_process(current, false);
            if (g_interrupts >= 2) host::stop_process(current, true);
        };
        int code = host::wait_child(current, forward);
        // Windows: a hot restart continues the program as another process.
        std::vector<int> waited{current};
        while (const uint32_t next = ctx.registry.successor(uint32_t(current), std::chrono::seconds(10))) {
            current = int(next);
            {
                std::lock_guard<std::mutex> lock(ctx.child.mutex);
                ctx.child.pid = current;
            }
            code = host::wait_child(current, forward);
            waited.push_back(current);
        }
        for (int p : waited) host::release_child(p);
        {
            std::lock_guard<std::mutex> lock(ctx.child.mutex);
            ctx.child.running = false;
            ctx.child.exit_code = code;
        }
        ctx.child.cv.notify_all();
        ctx.registry.notify();
        ctx.events.add(0, "exited", opt.program[0] + " exited with code " + std::to_string(code));
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            server.cv.wait_for(lock, std::chrono::seconds(2), [&] { return server.agents == 0; });
        }
        std::printf("[hotpp] %s exited with code %d\n", opt.program[0].c_str(), code);
        std::fflush(stdout);
        // Started by hotpp mcp: stay, so the agent can still read the output and
        // events, until it lets go (stop, or it exits).
        if (ctx.lifeline) {
            std::unique_lock<std::mutex> lock(ctx.child.mutex);
            ctx.child.cv.wait(lock, [&] { return ctx.child.dismissed || lifeline_lost.load(); });
        }
        shut_down(*listener, acceptor, server);
        wake.set();
        trace("waiting for the output reader");
        if (output.joinable()) output.join();
        trace("waiting for helpers");
        join_helpers();
        trace("done");
        if (code != 0) return code;
        if (server.sessions_ended == 0) {
            std::fprintf(stderr, "[hotpp] error: no agent connected\n");
            return 2;
        }
        return server.problems ? 3 : 0;
    }

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        while (!g_stop && !(opt.once && server.sessions_ended > 0))
            server.cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    if (g_stop) std::printf("[hotpp] shutting down\n");
    shut_down(*listener, acceptor, server);
    join_helpers();
    return server.problems ? 3 : 0;
}
