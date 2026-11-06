// The HotPP hosts hotpp mcp works with: the ones it finds on this machine (any
// host the user started, or one an agent library launched) and the ones it
// launched itself, each with a program. Talks to them over the control channel
// (host/control_protocol.h).
#pragma once

#include "host/platform.h"
#include "host/util/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hotpp::mcp {

struct HostError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One control connection. Thread-safe: requests from several threads share it.
class HostClient {
public:
    // Connects and exchanges hellos. nullptr with `error` set if nothing answers
    // there; `incompatible` if something answered but isn't a host we can control
    // (e.g. a HotPP 1.0 host).
    static std::shared_ptr<HostClient> connect(const std::string& endpoint, std::chrono::milliseconds timeout,
                                               std::string& error, bool& incompatible);
    ~HostClient();

    const std::string& endpoint() const { return endpoint_; }
    const json::Value& hello() const { return hello_; } // the host as it described itself
    int64_t host_pid() const;
    bool alive() const { return alive_.load(); }

    // Sends a request and waits for its result. Throws HostError with the host's
    // message, or on timeout, cancellation or a lost connection. `progress` gets
    // the host's progress lines.
    json::Value request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                        const std::function<void(const std::string&)>& progress = {},
                        const std::atomic<bool>* cancelled = nullptr);
    void close();

private:
    HostClient() = default;
    void read_loop();

    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool closed = false; // the requester stopped waiting: no more progress calls
        json::Value reply;
        std::function<void(const std::string&)> progress;
    };

    std::string endpoint_;
    std::unique_ptr<host::Stream> stream_;
    std::thread reader_;
    std::atomic<bool> alive_{true};
    json::Value hello_;
    std::mutex send_mutex_;
    std::mutex mutex_;
    std::condition_variable hello_cv_;
    bool got_hello_ = false;
    std::map<int64_t, std::shared_ptr<Pending>> pending_;
    int64_t next_id_ = 1;
};

// A host this server started (hotpp --lifeline ... -- <program>).
struct Launched {
    std::string endpoint;
    int host_pid = -1;
    std::vector<std::string> command;
    std::string cwd;
    int lifeline = -1; // write end (under `mutex`); closing it stops the host and its program

    std::string host_output() const; // the last 64 KiB the host printed
    bool exited() const;
    int exit_status() const; // the host's exit code (128 + signal if killed)

    // Internal.
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::string output;
    bool done = false;
    int status = -1;
    std::thread reader, reaper;
};

struct LaunchSpec {
    std::vector<std::string> command;
    std::string cwd;
    std::vector<std::string> env; // NAME=value
    std::string build_command;
    bool auto_restart = false;
    bool auto_reload = false;
};

struct HostsOptions {
    std::vector<std::string> endpoints; // besides the ones in the runtime directory
    bool allow_launch = true;
};

class Hosts {
public:
    explicit Hosts(HostsOptions options);
    ~Hosts();

    struct Entry {
        std::string endpoint;
        std::shared_ptr<HostClient> client; // null if unreachable
        std::string error;                  // why it's unreachable
        bool incompatible = false;
        std::shared_ptr<Launched> launched; // started by this server
    };
    // Every host found, connected where possible.
    std::vector<Entry> discover();

    struct Program {
        std::shared_ptr<HostClient> host;
        std::shared_ptr<Launched> launched;
        uint32_t pid = 0;
        json::Value info;     // the session (SessionInfo json); null if `exited`
        json::Value host_info; // the host's status["host"]
        bool exited = false;  // a launched program that is no longer running
    };
    // Every program connected to a reachable host, plus launched ones that exited.
    // `status` holds the raw status replies, by endpoint.
    std::vector<Program> programs(std::vector<Entry>* hosts = nullptr);
    // The program `pid` names, or the only one there is (preferring the ones this
    // server launched). Throws HostError explaining the choice otherwise.
    Program find(std::optional<uint32_t> pid, bool include_exited = false);

    // Starts a host running spec.command. Throws HostError if it doesn't come up.
    std::shared_ptr<Launched> launch(const LaunchSpec& spec, std::shared_ptr<HostClient>& client,
                                     std::chrono::milliseconds timeout);
    // Lets go of a launched host (after it exits).
    void forget(const std::shared_ptr<Launched>& launched);

    // Stops every launched host and its program (closing their lifelines), waiting
    // up to `grace` before killing what's left.
    void stop_all(std::chrono::milliseconds grace);

    // The newest event sequence number a reload has reported for a program, so the
    // next reload only reports what's new.
    uint64_t seen(const std::string& endpoint, int64_t host_pid, uint32_t pid);
    void set_seen(const std::string& endpoint, int64_t host_pid, uint32_t pid, uint64_t seq);

    bool allow_launch() const { return options_.allow_launch; }

private:
    std::vector<std::string> candidate_endpoints();
    std::shared_ptr<Launched> launched_at(const std::string& endpoint);

    HostsOptions options_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<HostClient>> clients_;
    std::map<std::string, std::string> incompatible_; // endpoint -> identity of the socket file then
    std::vector<std::shared_ptr<Launched>> launched_;
    std::map<std::string, uint64_t> seen_;
    std::atomic<int> next_launch_{1};
};

} // namespace hotpp::mcp
