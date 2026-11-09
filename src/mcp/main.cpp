// hotpp mcp: reads JSON-RPC messages from stdin, one per line, and writes replies
// to stdout. Nothing else may reach stdout, so it is moved aside at startup and
// fd 1 points at stderr for anything that prints by accident.
#include "mcp/mcp.h"

#include "host/platform.h"
#include "mcp/hosts.h"
#include "mcp/server.h"
#include "mcp/tools.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    "  --build <command>   default rebuild command for launched programs (run with the shell)\n"
    "  --endpoint <path>   also control the host listening there (repeatable)\n"
    "  --no-launch         don't start, stop or restart programs; only observe and patch\n"
    "  --structured        also return structuredContent (for scripts; agents read the text)\n"
    "  --version           print the version and exit\n";

constexpr size_t kMaxLine = 16u << 20;

// Set by a stop request (Ctrl+C, SIGTERM, SIGHUP): ends the server like end of input.
std::atomic<bool> g_stopping{false};
host::WakeEvent* g_wake = nullptr;

void on_stop(host::StopRequest) {
    g_stopping = true;
    if (g_wake) g_wake->set();
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

    // The protocol gets a private copy of stdout; stdout itself becomes stderr.
    std::fflush(stdout);
    std::string error;
    const host::Handle out = host::take_stdout(error);
    if (out == host::kNoHandle) {
        std::fprintf(stderr, "hotpp mcp: %s\n", error.c_str());
        return 1;
    }

    // A closed stdout is noticed by write; stop requests end the server like end of input.
    host::WakeEvent wake;
    g_wake = &wake;
    host::install_signal_handlers(on_stop, nullptr);
    std::atomic<bool>& stopping = g_stopping;

    std::mutex out_mutex;
    std::atomic<bool> out_broken{false};
    auto write_line = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(out_mutex);
        if (out_broken) return;
        std::string msg = line;
        msg += '\n';
        if (!host::write_all(out, msg.data(), msg.size())) {
            out_broken = true;
            wake.set();
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
        const host::Handle in = host::stdin_handle();
        while (!stopping && !out_broken) {
            const long n = host::read_some(in, chunk, sizeof chunk, -1, &wake);
            if (n == -2) break; // woken: stopping or stdout gone
            if (n <= 0) break;  // end of input: the client is done
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
        while (!stopping && !out_broken && !server.idle()) wake.wait(100);
        server.shutdown();
        hosts.stop_all(std::chrono::seconds(6)); // launched programs end with this server
        if (out_broken) exit_code = 0;          // the client went away; nothing to report to
    }
    g_wake = nullptr;
    host::close_handle(out);
    return exit_code;
}

} // namespace hotpp::mcp
