#include "host/platform.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>

#include <csignal>

#include <fcntl.h>
#include <spawn.h>
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

    int64_t peer_uid() const override {
        ucred cred{};
        socklen_t len = sizeof cred;
        if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return -1;
        return int64_t(cred.uid);
    }

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
    for (const auto& [from, to] : opt.fds) posix_spawn_file_actions_adddup2(&actions, from, to);
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

int spawn_child_pty(const std::vector<std::string>& args, int& master_fd, std::string& error) {
    master_fd = -1;
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
    opt.fds = {{slave, STDOUT_FILENO}, {slave, STDERR_FILENO}};
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
    opt.fds = {{fds[1], STDOUT_FILENO}, {fds[1], STDERR_FILENO}};
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

} // namespace hotpp::host
