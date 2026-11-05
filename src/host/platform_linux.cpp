#include "host/platform.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hotpp::host {

namespace {

class UnixStream final : public Stream {
public:
    explicit UnixStream(int fd) : fd_(fd) {}
    ~UnixStream() override { ::close(fd_); }

    void shutdown() override { ::shutdown(fd_, SHUT_RDWR); }

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

int spawn_child(const std::vector<std::string>& args, std::string& error) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        error = errno_text(("cannot start " + args[0]).c_str(), rc);
        return -1;
    }
    return int(pid);
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
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());

    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        error = errno_text(("cannot start " + args[0]).c_str(), rc);
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
