// OS layer of the host (platform_linux.cpp, platform_win.cpp). Shared code calls
// only this, never the OS directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hotpp::host {

// A file descriptor (Linux) or a HANDLE (Windows).
using Handle = intptr_t;
constexpr Handle kNoHandle = -1;

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
    // Whether the process at the other end runs as the same user as this one.
    virtual bool peer_is_same_user() const { return false; }
    // Describes the peer for messages (e.g. "uid 1001"); empty if unknown.
    virtual std::string peer_description() const { return {}; }
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
// Endpoints of hosts that may be listening: `<prefix>` and `<prefix>-*` (see
// common/endpoint.h) that exist right now.
std::vector<std::string> list_endpoints(const std::string& prefix);
// Identifies the object behind an endpoint, so one that answered wrongly isn't
// probed again until it is replaced. Empty if it doesn't exist.
std::string endpoint_identity(const std::string& endpoint);

// A signal another thread can wait for: set() wakes read_some() and wait().
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    // Safe to call from a signal handler.
    void set() const;
    void reset() const;
    // Waits until set or `timeout_ms` passes (-1: forever); true if set.
    bool wait(int timeout_ms) const;
    Handle native() const { return handle_; }

private:
    Handle handle_ = kNoHandle;
};

// Reads up to `size` bytes, waiting at most `timeout_ms` (-1: forever) and less if
// `wake` is set. Returns the bytes read, 0 at end of file, -1 on an error and -2 if
// it timed out or was woken.
long read_some(Handle h, void* data, size_t size, int timeout_ms = -1, const WakeEvent* wake = nullptr);
bool write_all(Handle h, const void* data, size_t size);
void close_handle(Handle h);

struct Pipe {
    Handle read = kNoHandle;
    Handle write = kNoHandle;
};
// Neither end is inherited by child processes unless passed in SpawnOptions::handles.
bool make_pipe(Pipe& pipe, std::string& error);

struct SpawnOptions {
    std::filesystem::path cwd;             // empty: the current directory
    std::string program;                   // run this file instead of argv[0] searched on PATH
    // (handle, n): the child gets `handle` as its stdin (0), stdout (1), stderr (2),
    // or, for n >= 3, as an extra inherited handle; handle_argument() says how the
    // child names it.
    std::vector<std::pair<Handle, int>> handles;
    bool null_stdin = false;               // stdin from the null device
    std::vector<std::string> env;          // NAME=value entries added to (or replacing) the environment
    bool new_process_group = false;        // so Ctrl+C sent to ours doesn't reach it
};
// Starts a process with SIGPIPE at its default and no signals blocked, whatever the
// caller has. Returns its pid, or -1 (with `error` set).
int spawn_process(const std::vector<std::string>& argv, const SpawnOptions& options, std::string& error);
// How a child refers to the extra handle passed as number `n` (n >= 3) of
// SpawnOptions::handles: a command-line argument it gives back to parse_handle().
std::string handle_argument(Handle h, int n);
Handle parse_handle(const std::string& argument);
// Makes a handle we were given (parse_handle) private: not inherited by our children.
void keep_private(Handle h);

// Starts argv[0] (searched on PATH) with the host's stdio. Returns its pid, or -1
// (with `error` set) if it couldn't start.
int spawn_child(const std::vector<std::string>& argv, std::string& error);
// Like spawn_child, but stdout and stderr are a pseudo-terminal (so the program
// buffers its output by line, as on a terminal) and stdin is the null device. Its
// other end is returned in `output`: reading it gives the output, until end of file
// (or an error) once the program and everything it started have closed it.
int spawn_child_pty(const std::vector<std::string>& argv, Handle& output, std::string& error);
// Waits for a child started by spawn_*() and returns its exit code (128 + signal
// number if killed by a signal). `on_signal` runs whenever a stop request
// interrupts the wait, e.g. to forward it.
int wait_child(int pid, const std::function<void()>& on_signal);
// Windows: a hot restart continued child `old_pid` as process `new_pid`, which
// takes over its pseudo-console and job; wait_child() then works for it. Linux
// keeps the pid: no-op.
void adopt_child(int old_pid, int new_pid);
// Frees what a child waited for leaves behind (Windows: its pseudo-console).
void release_child(int pid);
// Asks a child to stop (SIGTERM; Ctrl+Break on Windows), or kills it (`force`).
void stop_process(int pid, bool force);
// Kills a child started with new_process_group and everything in its group.
void kill_process_group(int pid);
// Whether a process with this pid exists (any process, not only children).
bool process_alive(uint32_t pid);

// Runs argv[0] in `cwd` with stdout and stderr captured into `output`, waits for it
// and returns its exit code, or -1 (with `error` set) if it couldn't start.
int run_capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                std::string& output, std::string& error);
// argv that runs `command` with the system shell (/bin/sh -c, cmd.exe /c).
std::vector<std::string> shell_command(const std::string& command);
// "/bin/sh -c" or "cmd.exe /c", for help texts.
const char* shell_description();

bool set_env(const char* name, const std::string& value);
uint32_t current_pid();
std::filesystem::path self_executable();
bool stdout_is_terminal();
// Readable C++ name for a mangled symbol; the name itself if it can't be demangled.
std::string demangle(const std::string& name);

enum class StopRequest { Interrupt, Terminate };
// Ctrl+C (Interrupt), SIGTERM/SIGHUP or closing the console (Terminate) call
// `on_stop`; SIGUSR1 calls `on_restart` (no equivalent on Windows). Both run in a
// signal handler: they may only touch atomics and WakeEvent::set(). A closed stdout
// never kills the process.
void install_signal_handlers(void (*on_stop)(StopRequest), void (*on_restart)());
// How to ask a running host to restart its programs, for messages.
const char* restart_request_hint();

// Takes stdout for a protocol: returns a private handle to it and points stdout at
// stderr, so stray prints can't corrupt the protocol. kNoHandle (with `error`) on failure.
Handle take_stdout(std::string& error);
Handle stdin_handle();

} // namespace hotpp::host
