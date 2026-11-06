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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

using namespace hotpp;

constexpr const char* kUsage =
    "usage: hotpp [options] [--once]\n"
    "       hotpp [options] -- <program> [args...]\n"
    "       hotpp mcp [mcp options]    (see hotpp mcp --help)\n"
    "\n"
    "Without a program, waits for agents to connect (--once: exit after the first\n"
    "one disconnects). With a program, starts it connected to this host and exits\n"
    "with its exit code; exits 2 if no agent connected, 3 if an agent reported an\n"
    "unusable memory layout. hotpp mcp serves AI agents over the Model Context\n"
    "Protocol.\n"
    "\n"
    "options:\n"
    "  --endpoint <path>   socket to listen on\n"
    "  --build <command>   rebuild command for hot restarts (run with /bin/sh -c)\n"
    "  --auto-restart      rebuild and restart when a change can't be patched\n"
    "  --no-auto-reload    don't patch saved files until a controller asks (hotpp mcp)\n"
    "  --capture-output    run the program on a pseudo-terminal and keep its output\n"
    "                      for controllers (it is still printed)\n"
    "  --lifeline <fd>     stop the program and exit once <fd> reads end of file\n"
    "                      (used by hotpp mcp)\n"
    "  -v, --verbose       full connect info and every binding decision\n"
    "  --version           print the version and exit\n"
    "\n"
    "kill -USR1 <host pid> rebuilds and restarts every connected program.\n";

struct Options {
    std::string endpoint;
    bool once = false;
    bool version = false;
    bool capture_output = false;
    int lifeline = -1;
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
            char* end = nullptr;
            const long fd = std::strtol(argv[++i], &end, 10);
            if (!end || *end || fd < 3 || fd > 1 << 20) return false;
            o.lifeline = int(fd);
        } else {
            return false;
        }
    }
    return true;
}

// Set by SIGINT/SIGTERM: shut down cleanly (remove the socket, end sessions).
std::atomic<int> g_stop_signal{0};
std::atomic<int> g_interrupts{0};

void on_stop_signal(int sig) {
    g_stop_signal = sig;
    if (sig == SIGINT) g_interrupts.fetch_add(1);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop_signal; // no SA_RESTART: interrupt waitpid() so signals can be forwarded
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sa.sa_handler = [](int) { host::g_restart_requests.fetch_add(1); };
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);
    // A closed stdout (hotpp mcp gone, `| head`) must not kill the host; children
    // get SIGPIPE back at its default (spawn_process).
    signal(SIGPIPE, SIG_IGN);
}

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
    if (stream.peer_uid() != int64_t(::getuid())) {
        std::fprintf(stderr, "[hotpp] refused a controller of another user (uid %lld)\n", (long long)stream.peer_uid());
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
    listener.close();
    if (acceptor.joinable()) acceptor.join();
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        for (auto& [s, controller] : server.open) controller ? s->shutdown_read() : s->shutdown();
    }
    for (std::thread& t : server.threads) t.join();
}

void wake(int fd) {
    const uint64_t one = 1;
    ssize_t n = ::write(fd, &one, sizeof one);
    (void)n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "mcp") == 0) return hotpp::mcp::run(argc - 1, argv + 1);

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
    ctx.lifeline = opt.lifeline >= 0;
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
            ::kill(ctx.child.pid, SIGTERM); // not reaped yet (running), so the pid is still the program's
        }
        std::lock_guard<std::mutex> lock(killer_mutex);
        if (killer.joinable()) return;
        killer = std::thread([&ctx, grace] {
            std::unique_lock<std::mutex> lock(ctx.child.mutex);
            if (!ctx.child.cv.wait_for(lock, grace, [&] { return !ctx.child.running; })) ::kill(ctx.child.pid, SIGKILL);
        });
    };

    install_signal_handlers();
    std::printf("[hotpp] host %s listening on %s\n", HOTPP_VERSION, endpoint.c_str());
    std::fflush(stdout);

    // Wakes the lifeline and output threads at shutdown.
    const int wake_fd = ::eventfd(0, EFD_CLOEXEC);
    std::atomic<bool> lifeline_lost{false};
    std::thread lifeline;
    if (opt.lifeline >= 0) {
        ::fcntl(opt.lifeline, F_SETFD, FD_CLOEXEC); // not for the program
        lifeline = std::thread([&] {
            pollfd p[2] = {{opt.lifeline, POLLIN, 0}, {wake_fd, POLLIN, 0}};
            for (;;) {
                const int rc = ::poll(p, 2, -1);
                if (rc < 0 && errno == EINTR) continue;
                if (rc < 0 || (p[1].revents & POLLIN)) return; // shutting down
                char c;
                const ssize_t n = ::read(opt.lifeline, &c, 1);
                if (n > 0 || (n < 0 && (errno == EINTR || errno == EAGAIN))) continue;
                break; // end of file: whoever held the other end is gone
            }
            std::printf("[hotpp] the controller that started this host is gone; stopping\n");
            std::fflush(stdout);
            lifeline_lost = true;
            g_stop_signal = SIGTERM;
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
        wake(wake_fd);
        if (lifeline.joinable()) lifeline.join();
        std::lock_guard<std::mutex> lock(killer_mutex);
        if (killer.joinable()) killer.join();
    };

    if (run_mode) {
        int master = -1;
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
        if (master >= 0)
            output = std::thread([&, master] {
                char buf[8192];
                pollfd p[2] = {{master, POLLIN, 0}, {wake_fd, POLLIN, 0}};
                bool stopping = false;
                for (;;) {
                    const int rc = ::poll(p, 2, stopping ? 0 : -1);
                    if (rc < 0 && errno == EINTR) continue;
                    if (rc <= 0) break; // nothing more to read while stopping
                    if (p[1].revents & POLLIN) stopping = true; // read what's left, then stop
                    if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                        if (stopping) break;
                        continue;
                    }
                    const ssize_t n = ::read(master, buf, sizeof buf);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break; // EIO: every writer is gone
                    ctx.output->append(std::string_view(buf, size_t(n)));
                    std::fwrite(buf, 1, size_t(n), stdout);
                    std::fflush(stdout);
                }
                ctx.output->close();
                ::close(master);
            });

        // Ctrl+C reaches the child too (same process group); SIGTERM sent to the host
        // alone, or a second Ctrl+C, is passed on.
        const int code = host::wait_child(pid, [pid] {
            if (g_stop_signal == SIGTERM) kill(pid, SIGTERM);
            if (g_interrupts >= 2) kill(pid, SIGKILL);
        });
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
        wake(wake_fd);
        if (output.joinable()) output.join();
        join_helpers();
        if (code != 0) return code;
        if (server.sessions_ended == 0) {
            std::fprintf(stderr, "[hotpp] error: no agent connected\n");
            return 2;
        }
        return server.problems ? 3 : 0;
    }

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        while (!g_stop_signal && !(opt.once && server.sessions_ended > 0))
            server.cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    if (g_stop_signal) std::printf("[hotpp] shutting down\n");
    shut_down(*listener, acceptor, server);
    join_helpers();
    return server.problems ? 3 : 0;
}
