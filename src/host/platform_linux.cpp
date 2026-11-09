#include "host/platform.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

#include <csignal>

#include <cxxabi.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace hotpp::host {

namespace {

class UnixStream final : public Stream {
public:
    explicit UnixStream(int fd) : fd_(fd) {}
    ~UnixStream() override { ::close(fd_); }

    void shutdown() override { ::shutdown(fd_, SHUT_RDWR); }
    void shutdown_read() override { ::shutdown(fd_, SHUT_RD); }

    bool peer_is_same_user() const override { return peer_uid() == int64_t(::getuid()); }
    std::string peer_description() const override { return "uid " + std::to_string(peer_uid()); }

    bool send(const void* data, size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            ssize_t n = ::send(fd_, p, size, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += n;
            size -= size_t(n);
        }
        return true;
    }

    bool recv(void* data, size_t size) override {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            ssize_t n = ::recv(fd_, p, size, 0);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += n;
            size -= size_t(n);
        }
        return true;
    }

private:
    int64_t peer_uid() const {
        ucred cred{};
        socklen_t len = sizeof cred;
        if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
        return int64_t(cred.uid);
    }

    int fd_;
};

class UnixListener final : public Listener {
public:
    UnixListener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~UnixListener() override {
        close();
        ::close(fd_);
    }

    std::unique_ptr<Stream> accept() override {
        for (;;) {
            int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c >= 0) return std::make_unique<UnixStream>(c);
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return nullptr;
        }
    }

    void close() override {
        if (closed_.exchange(true)) return;
        ::unlink(path_.c_str());
        ::shutdown(fd_, SHUT_RDWR); // wakes a blocked accept()
    }

private:
    int fd_;
    std::string path_;
    std::atomic<bool> closed_{false};
};

std::string errno_text(const char* what, int err) {
    return std::string(what) + ": " + std::strerror(err);
}

} // namespace

std::unique_ptr<Listener> listen(const std::string& endpoint, std::string& error) {
    sockaddr_un addr{};
    if (endpoint.empty() || endpoint.size() >= sizeof addr.sun_path) {
        error = "endpoint path is empty or too long: " + endpoint;
        return nullptr;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.data(), endpoint.size());
    auto* sa = reinterpret_cast<sockaddr*>(&addr);

    // A live host on the endpoint is an error; a socket file left by a dead one is removed.
    int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        int rc = ::connect(probe, sa, sizeof addr);
        int err = errno;
        ::close(probe);
        if (rc == 0) {
            error = "another hotpp host is already listening on " + endpoint;
            return nullptr;
        }
        if (err == ECONNREFUSED) ::unlink(endpoint.c_str());
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = errno_text("socket", errno);
        return nullptr;
    }
    if (::bind(fd, sa, sizeof addr) != 0 || ::listen(fd, 16) != 0) {
        error = errno_text(("cannot listen on " + endpoint).c_str(), errno);
        ::close(fd);
        return nullptr;
    }
    ::chmod(endpoint.c_str(), 0600);
    return std::make_unique<UnixListener>(fd, endpoint);
}

std::unique_ptr<Stream> connect(const std::string& endpoint, std::string& error) {
    sockaddr_un addr{};
    if (endpoint.empty() || endpoint.size() >= sizeof addr.sun_path) {
        error = "endpoint path is empty or too long: " + endpoint;
        return nullptr;
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.data(), endpoint.size());
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = errno_text("socket", errno);
        return nullptr;
    }
    while (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (errno == EINTR) continue;
        error = errno_text(("cannot connect to " + endpoint).c_str(), errno);
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<UnixStream>(fd);
}

int spawn_process(const std::vector<std::string>& args, const SpawnOptions& opt, std::string& error) {
    if (args.empty()) {
        error = "no program to start";
        return -1;
    }
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // The environment, with opt.env replacing entries of the same name.
    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    char** env = environ;
    if (!opt.env.empty()) {
        auto name_of = [](const std::string& e) { return e.substr(0, e.find('=')); };
        std::set<std::string> replaced;
        for (const std::string& e : opt.env) replaced.insert(name_of(e));
        for (char** e = environ; e && *e; ++e)
            if (!replaced.count(name_of(*e))) env_storage.emplace_back(*e);
        for (const std::string& e : opt.env) env_storage.push_back(e);
        for (std::string& e : env_storage) envp.push_back(e.data());
        envp.push_back(nullptr);
        env = envp.data();
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (opt.null_stdin) posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    for (const auto& [from, to] : opt.handles) posix_spawn_file_actions_adddup2(&actions, int(from), to);
    if (!opt.cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, opt.cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaults, none;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setsigmask(&attr, &none);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    if (opt.new_process_group) {
        flags |= POSIX_SPAWN_SETPGROUP;
        posix_spawnattr_setpgroup(&attr, 0);
    }
    posix_spawnattr_setflags(&attr, flags);

    pid_t pid = 0;
    const int rc = opt.program.empty() ? posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), env)
                                       : posix_spawn(&pid, opt.program.c_str(), &actions, &attr, argv.data(), env);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        error = errno_text(("cannot start " + (opt.program.empty() ? args[0] : opt.program)).c_str(), rc);
        return -1;
    }
    return int(pid);
}

int spawn_child(const std::vector<std::string>& args, std::string& error) {
    return spawn_process(args, {}, error);
}

int spawn_child_pty(const std::vector<std::string>& args, Handle& master_fd, std::string& error) {
    master_fd = kNoHandle;
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        error = errno_text("posix_openpt", errno);
        return -1;
    }
    char name[128];
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0 || ::ptsname_r(master, name, sizeof name) != 0) {
        error = errno_text("pseudo-terminal", errno);
        ::close(master);
        return -1;
    }
    const int slave = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        error = errno_text(("cannot open " + std::string(name)).c_str(), errno);
        ::close(master);
        return -1;
    }
    // Output as written: no "\n" -> "\r\n", no echo. A wide window, so programs
    // that wrap their output to the terminal don't wrap much.
    termios t{};
    if (::tcgetattr(slave, &t) == 0) {
        t.c_oflag &= ~tcflag_t(OPOST);
        t.c_lflag &= ~tcflag_t(ECHO | ECHONL);
        ::tcsetattr(slave, TCSANOW, &t);
    }
    winsize ws{};
    ws.ws_row = 50;
    ws.ws_col = 200;
    ::ioctl(master, TIOCSWINSZ, &ws);

    SpawnOptions opt;
    opt.null_stdin = true;
    opt.handles = {{slave, STDOUT_FILENO}, {slave, STDERR_FILENO}};
    const int pid = spawn_process(args, opt, error);
    ::close(slave);
    if (pid < 0) {
        ::close(master);
        return -1;
    }
    master_fd = master;
    return pid;
}

int wait_child(int pid, const std::function<void()>& on_signal) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
        if (on_signal) on_signal();
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int run_capture(const std::vector<std::string>& args, const std::filesystem::path& cwd,
                std::string& output, std::string& error) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        error = errno_text("pipe", errno);
        return -1;
    }
    SpawnOptions opt;
    opt.cwd = cwd;
    opt.null_stdin = true;
    opt.handles = {{fds[1], STDOUT_FILENO}, {fds[1], STDERR_FILENO}};
    const int pid = spawn_process(args, opt, error);
    ::close(fds[1]);
    if (pid < 0) {
        ::close(fds[0]);
        return -1;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) {
            output.append(buf, size_t(n));
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            error = errno_text("waitpid", errno);
            return -1;
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

bool set_env(const char* name, const std::string& value) {
    return ::setenv(name, value.c_str(), 1) == 0;
}

uint32_t current_pid() { return uint32_t(::getpid()); }

// ---- endpoints ---------------------------------------------------------------

std::vector<std::string> list_endpoints(const std::string& prefix) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const fs::path p = prefix;
    const std::string base = p.filename().string();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        const bool ours = name == base + ".sock" ||
                          (name.rfind(base + "-", 0) == 0 && name.size() > 5 && name.compare(name.size() - 5, 5, ".sock") == 0);
        std::error_code sec;
        if (ours && e.is_socket(sec)) out.push_back(e.path().string());
    }
    return out;
}

std::string endpoint_identity(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};
    return std::to_string(st.st_ino) + ":" + std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec);
}

// ---- events and pipes --------------------------------------------------------

WakeEvent::WakeEvent() : handle_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

WakeEvent::~WakeEvent() {
    if (handle_ >= 0) ::close(int(handle_));
}

void WakeEvent::set() const {
    const uint64_t one = 1;
    ssize_t n = ::write(int(handle_), &one, sizeof one);
    (void)n;
}

void WakeEvent::reset() const {
    uint64_t v;
    ssize_t n = ::read(int(handle_), &v, sizeof v);
    (void)n;
}

bool WakeEvent::wait(int timeout_ms) const {
    pollfd p{int(handle_), POLLIN, 0};
    for (;;) {
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0;
    }
}

long read_some(Handle h, void* data, size_t size, int timeout_ms, const WakeEvent* wake) {
    pollfd p[2] = {{int(h), POLLIN, 0}, {wake ? int(wake->native()) : -1, POLLIN, 0}};
    for (;;) {
        const int rc = ::poll(p, wake ? 2 : 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return -2;
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) return -2; // woken
        const ssize_t n = ::read(int(h), data, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) return errno == EIO ? 0 : -1; // EIO: a pseudo-terminal whose writers are all gone
        return long(n);
    }
}

bool write_all(Handle h, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(int(h), p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= size_t(n);
    }
    return true;
}

void close_handle(Handle h) {
    if (h >= 0) ::close(int(h));
}

bool make_pipe(Pipe& pipe, std::string& error) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        error = errno_text("pipe", errno);
        return false;
    }
    // Children get handles on fixed low numbers (dup2 in spawn_process); keep ours
    // clear of those.
    auto high = [](int fd) {
        const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
        ::close(fd);
        return moved;
    };
    pipe.read = high(fds[0]);
    pipe.write = high(fds[1]);
    return pipe.read >= 0 && pipe.write >= 0;
}

std::string handle_argument(Handle, int n) { return std::to_string(n); }

Handle parse_handle(const std::string& argument) {
    char* end = nullptr;
    const long fd = std::strtol(argument.c_str(), &end, 10);
    if (!end || *end || fd < 3 || fd > 1 << 20) return kNoHandle;
    return Handle(fd);
}

void keep_private(Handle h) { ::fcntl(int(h), F_SETFD, FD_CLOEXEC); }

// ---- processes ---------------------------------------------------------------

void adopt_child(int, int) {}
void release_child(int) {}

void stop_process(int pid, bool force) { ::kill(pid, force ? SIGKILL : SIGTERM); }

void kill_process_group(int pid) { ::kill(-pid, SIGKILL); }

bool process_alive(uint32_t pid) { return pid && (::kill(pid_t(pid), 0) == 0 || errno == EPERM); }

std::vector<std::string> shell_command(const std::string& command) { return {"/bin/sh", "-c", command}; }

const char* shell_description() { return "/bin/sh -c"; }

std::filesystem::path self_executable() {
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof self - 1);
    return n > 0 ? std::filesystem::path(std::string(self, size_t(n))) : std::filesystem::path("hotpp");
}

bool stdout_is_terminal() { return ::isatty(STDOUT_FILENO); }

std::string demangle(const std::string& name) {
    int status = 0;
    char* d = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    std::string out = status == 0 && d ? d : name;
    std::free(d);
    return out;
}

// ---- signals and stdio -------------------------------------------------------

namespace {
void (*g_on_stop)(StopRequest) = nullptr;
void (*g_on_restart)() = nullptr;

void stop_handler(int sig) {
    if (g_on_stop) g_on_stop(sig == SIGINT ? StopRequest::Interrupt : StopRequest::Terminate);
}
} // namespace

void install_signal_handlers(void (*on_stop)(StopRequest), void (*on_restart)()) {
    g_on_stop = on_stop;
    g_on_restart = on_restart;
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = stop_handler; // no SA_RESTART: interrupt waitpid() so signals can be forwarded
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    if (on_restart) {
        sa.sa_handler = [](int) {
            if (g_on_restart) g_on_restart();
        };
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, nullptr);
    }
    // A closed stdout (hotpp mcp gone, `| head`) must not kill us; children get
    // SIGPIPE back at its default (spawn_process).
    signal(SIGPIPE, SIG_IGN);
}

const char* restart_request_hint() { return "send the host SIGUSR1"; }

Handle take_stdout(std::string& error) {
    const int out = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (out < 0) {
        error = errno_text("stdout", errno);
        return kNoHandle;
    }
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    return out;
}

Handle stdin_handle() { return STDIN_FILENO; }

} // namespace hotpp::host
