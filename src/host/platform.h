// OS layer of the host (platform_linux.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::host {

// One agent connection. send/recv transfer exactly `size` bytes or fail.
class Stream {
public:
    virtual ~Stream() = default;
    virtual bool send(const void* data, size_t size) = 0;
    virtual bool recv(void* data, size_t size) = 0;
    // Ends the connection from another thread: a blocked recv() returns false.
    virtual void shutdown() = 0;
    // Ends only the receiving side: a blocked recv() returns false, send() still works.
    virtual void shutdown_read() { shutdown(); }
    // The user id of the process at the other end, or -1 if unknown.
    virtual int64_t peer_uid() const { return -1; }
};

class Listener {
public:
    virtual ~Listener() = default;
    // Blocks until an agent connects; nullptr once the listener is closed or broken.
    virtual std::unique_ptr<Stream> accept() = 0;
    // Stops accepting and removes the endpoint. Safe to call from another thread.
    virtual void close() = 0;
};

std::unique_ptr<Listener> listen(const std::string& endpoint, std::string& error);
// Connects to a listening host; nullptr (with `error` set) if nobody listens there.
std::unique_ptr<Stream> connect(const std::string& endpoint, std::string& error);

struct SpawnOptions {
    std::filesystem::path cwd;             // empty: the current directory
    std::string program;                   // run this file instead of argv[0] searched on PATH
    std::vector<std::pair<int, int>> fds;  // dup2(first, second) in the child
    bool null_stdin = false;               // stdin from /dev/null
    std::vector<std::string> env;          // NAME=value entries added to (or replacing) the environment
    bool new_process_group = false;        // so terminal signals (Ctrl+C) sent to ours don't reach it
};
// Starts a process with SIGPIPE at its default and no signals blocked, whatever the
// caller has. Returns its pid, or -1 (with `error` set).
int spawn_process(const std::vector<std::string>& argv, const SpawnOptions& options, std::string& error);

// Starts argv[0] (searched on PATH) with the host's stdio. Returns its pid, or -1
// (with `error` set) if it couldn't start.
int spawn_child(const std::vector<std::string>& argv, std::string& error);
// Like spawn_child, but stdout and stderr are a pseudo-terminal (so the program
// buffers its output by line, as on a terminal) and stdin is /dev/null. Its other
// end is returned in `master_fd`: reading it gives the output, until EIO once the
// program and everything it started have closed it.
int spawn_child_pty(const std::vector<std::string>& argv, int& master_fd, std::string& error);
// Waits for a child started by spawn_child() and returns its exit code (128 +
// signal number if killed by a signal). `on_signal` runs whenever a signal
// interrupts the wait, e.g. to forward it.
int wait_child(int pid, const std::function<void()>& on_signal);

// Runs argv[0] in `cwd` with stdout and stderr captured into `output`, waits for it
// and returns its exit code, or -1 (with `error` set) if it couldn't start.
int run_capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                std::string& output, std::string& error);

bool set_env(const char* name, const std::string& value);
uint32_t current_pid();

} // namespace hotpp::host
