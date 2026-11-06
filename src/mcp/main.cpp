// hotpp mcp: reads JSON-RPC messages from stdin, one per line, and writes replies
// to stdout. Nothing else may reach stdout, so it is moved aside at startup and
// fd 1 points at stderr for anything that prints by accident.
#include "mcp/mcp.h"

#include "mcp/hosts.h"
#include "mcp/server.h"
#include "mcp/tools.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace hotpp::mcp {

namespace {

constexpr const char* kUsage =
    "usage: hotpp mcp [options]\n"
    "\n"
    "A Model Context Protocol server (stdio) for AI coding agents: it lets them launch\n"
    "programs under HotPP, apply source changes, read compile errors and output,\n"
    "restart and stop. It also sees programs running under hosts you started.\n"
    "\n"
    "  claude mcp add hotpp -- hotpp mcp --build \"cmake --build build\"\n"
    "\n"
    "options:\n"
    "  --build <command>   default rebuild command for launched programs (/bin/sh -c)\n"
    "  --endpoint <path>   also control the host listening there (repeatable)\n"
    "  --no-launch         don't start, stop or restart programs; only observe and patch\n"
    "  --structured        also return structuredContent (for scripts; agents read the text)\n"
    "  --version           print the version and exit\n";

constexpr size_t kMaxLine = 16u << 20;

// Writes all of `data` to `fd`; false if it's gone.
bool write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        size -= size_t(n);
    }
    return true;
}

} // namespace

int run(int argc, char** argv) {
    HostsOptions hosts_options;
    ToolsOptions tools_options;
    bool restricted = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--build" && i + 1 < argc) {
            tools_options.default_build = argv[++i];
        } else if (a == "--endpoint" && i + 1 < argc) {
            hosts_options.endpoints.push_back(argv[++i]);
        } else if (a == "--no-launch") {
            restricted = true;
        } else if (a == "--structured") {
            tools_options.structured = true;
        } else if (a == "--version") {
            std::printf("hotpp mcp %s\n", HOTPP_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "hotpp mcp: unknown option '%s'\n\n%s", a.c_str(), kUsage);
            return 1;
        }
    }
    hosts_options.allow_launch = !restricted;
    std::error_code ec;
    tools_options.cwd = std::filesystem::current_path(ec).string();

    // The protocol gets a private copy of stdout; fd 1 becomes stderr.
    std::fflush(stdout);
    const int out = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (out < 0) {
        std::perror("hotpp mcp: stdout");
        return 1;
    }
    ::dup2(STDERR_FILENO, STDOUT_FILENO);

    // Signals: SIGPIPE ignored (a closed stdout is noticed by write), SIGINT,
    // SIGTERM and SIGHUP end the server like end of input. Blocked before any thread
    // starts, so only the waiter below takes them.
    signal(SIGPIPE, SIG_IGN);
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    const int wake = ::eventfd(0, EFD_CLOEXEC);
    std::atomic<bool> stopping{false};
    std::thread signal_waiter([&] {
        int sig = 0;
        sigwait(&stop_signals, &sig);
        stopping = true;
        const uint64_t one = 1;
        ssize_t n = ::write(wake, &one, sizeof one);
        (void)n;
    });

    std::mutex out_mutex;
    std::atomic<bool> out_broken{false};
    auto write_line = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(out_mutex);
        if (out_broken) return;
        std::string msg = line;
        msg += '\n';
        if (!write_all(out, msg.data(), msg.size())) {
            out_broken = true;
            const uint64_t one = 1;
            ssize_t n = ::write(wake, &one, sizeof one);
            (void)n;
        }
    };

    int exit_code = 0;
    {
        Hosts hosts(hosts_options);
        HotppTools tools(hosts, tools_options);
        ServerInfo info{"hotpp", "HotPP", HOTPP_VERSION, instructions()};
        Server server(tools, info, write_line);
        std::fprintf(stderr, "[hotpp mcp] %s ready\n", HOTPP_VERSION);

        // Lines from stdin; a line longer than kMaxLine is discarded up to its newline.
        std::string buffer;
        bool discarding = false;
        char chunk[65536];
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {wake, POLLIN, 0}};
        while (!stopping && !out_broken) {
            const int rc = ::poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (fds[1].revents & POLLIN) break;
            if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break; // end of input: the client is done
            size_t start = 0;
            for (size_t i = 0; i < size_t(n); ++i) {
                if (chunk[i] != '\n') continue;
                if (!discarding) {
                    buffer.append(chunk + start, i - start);
                    server.handle_line(buffer);
                }
                buffer.clear();
                discarding = false;
                start = i + 1;
            }
            if (!discarding) buffer.append(chunk + start, size_t(n) - start);
            if (!discarding && buffer.size() > kMaxLine) {
                server.line_too_long();
                buffer.clear();
                discarding = true;
            }
        }
        if (!discarding && !buffer.empty() && !stopping) server.handle_line(buffer); // a last line without newline
        // End of input: let calls in progress finish and answer (the client may still
        // read), unless a signal says to stop now.
        while (!stopping && !out_broken && !server.idle()) {
            pollfd w{wake, POLLIN, 0};
            ::poll(&w, 1, 100);
        }
        server.shutdown();
        hosts.stop_all(std::chrono::seconds(6)); // launched programs end with this server
        if (out_broken) exit_code = 0;          // the client went away; nothing to report to
    }
    // Wake the signal waiter so it can be joined.
    pthread_kill(signal_waiter.native_handle(), SIGTERM);
    signal_waiter.join();
    ::close(out);
    return exit_code;
}

} // namespace hotpp::mcp
