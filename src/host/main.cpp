// hotpp: the HotPP host. Agents in running programs connect to it; it watches their
// sources, recompiles what changes and sends patches.
//
//   hotpp [--endpoint <name>] [--once] [-v]      serve: wait for agents
//   hotpp [--endpoint <name>] [-v] -- <program>   run: start <program> connected to this host
#include "common/endpoint.h"
#include "host/platform.h"
#include "host/session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hotpp;

constexpr const char* kUsage =
    "usage: hotpp [options] [--once]\n"
    "       hotpp [options] -- <program> [args...]\n"
    "\n"
    "Without a program, waits for agents to connect (--once: exit after the first\n"
    "one disconnects). With a program, starts it connected to this host and exits\n"
    "with its exit code; exits 2 if no agent connected, 3 if an agent reported an\n"
    "unusable memory layout.\n"
    "\n"
    "options:\n"
    "  --endpoint <path>   socket to listen on\n"
    "  --build <command>   rebuild command for hot restarts (run with /bin/sh -c)\n"
    "  --auto-restart      rebuild and restart when a change can't be patched\n"
    "  -v, --verbose       full connect info and every binding decision\n"
    "  --version           print the version and exit\n"
    "\n"
    "kill -USR1 <host pid> rebuilds and restarts every connected program.\n";

struct Options {
    std::string endpoint;
    bool once = false;
    bool version = false;
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
}

struct Server {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0;         // connections being handled
    int sessions_ended = 0; // agents that sent a hello and then disconnected
    int problems = 0;       // agents with an unusable layout
    host::HostOptions options;
    std::vector<std::thread> sessions; // joined at shutdown
    std::set<host::Stream*> open;      // live connections, ended at shutdown
};

void handle_client(std::unique_ptr<host::Stream> stream, Server* server) {
    host::Stream* raw = stream.get();
    host::Session::Outcome outcome;
    {
        host::Session session(std::move(stream), server->options);
        outcome = session.run();
        std::lock_guard<std::mutex> lock(server->mutex);
        server->open.erase(raw); // before the session (and its stream) goes away
    }
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        --server->active;
        server->sessions_ended += outcome.hello;
        server->problems += outcome.hello && !outcome.layout_ok;
    }
    server->cv.notify_all();
}

// Stops accepting, ends every open connection and waits for all session threads.
void shut_down(host::Listener& listener, std::thread& acceptor, Server& server) {
    listener.close();
    if (acceptor.joinable()) acceptor.join();
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        for (host::Stream* s : server.open) s->shutdown();
    }
    for (std::thread& t : server.sessions) t.join();
}

} // namespace

int main(int argc, char** argv) {
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
    server.options = opt.host;
    install_signal_handlers();
    std::printf("[hotpp] host %s listening on %s\n", HOTPP_VERSION, endpoint.c_str());
    std::fflush(stdout);

    std::thread acceptor([&] {
        while (std::unique_ptr<host::Stream> s = listener->accept()) {
            std::lock_guard<std::mutex> lock(server.mutex);
            ++server.active;
            server.open.insert(s.get());
            server.sessions.emplace_back(handle_client, std::move(s), &server);
        }
    });

    if (run_mode) {
        const int pid = host::spawn_child(opt.program, error);
        if (pid < 0) {
            std::fprintf(stderr, "[hotpp] error: %s\n", error.c_str());
            shut_down(*listener, acceptor, server);
            return 1;
        }
        // Ctrl+C reaches the child too (same process group); SIGTERM sent to the host
        // alone, or a second Ctrl+C, is passed on.
        const int code = host::wait_child(pid, [pid] {
            if (g_stop_signal == SIGTERM) kill(pid, SIGTERM);
            if (g_interrupts >= 2) kill(pid, SIGKILL);
        });
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            server.cv.wait_for(lock, std::chrono::seconds(2), [&] { return server.active == 0; });
        }
        shut_down(*listener, acceptor, server);
        std::printf("[hotpp] %s exited with code %d\n", opt.program[0].c_str(), code);
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
    return server.problems ? 3 : 0;
}
