#include "host/platform.h"
#include "common/win_util.h"

#include <windows.h>

#include <algorithm>
#include <mutex>

namespace hotpp::host {

namespace {

std::string last_error_text(const std::string& what) {
    return what + " (error " + std::to_string(GetLastError()) + ")";
}

class PipeStream final : public Stream {
public:
    explicit PipeStream(HANDLE h) : h_(h) {}
    ~PipeStream() override {
        DisconnectNamedPipe(h_);
        CloseHandle(h_);
    }

    bool send(const void* data, size_t size) override {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            DWORD done = 0;
            const DWORD chunk = DWORD(std::min<size_t>(size, size_t(1) << 20));
            if (!WriteFile(h_, p, chunk, &done, nullptr) || done == 0) return false;
            p += done;
            size -= done;
        }
        return true;
    }

    bool recv(void* data, size_t size) override {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            DWORD done = 0;
            const DWORD chunk = DWORD(std::min<size_t>(size, size_t(1) << 20));
            if (!ReadFile(h_, p, chunk, &done, nullptr) || done == 0) return false;
            p += done;
            size -= done;
        }
        return true;
    }

private:
    HANDLE h_;
};

HANDLE create_instance(const std::wstring& name, bool first) {
    DWORD open_mode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(name.c_str(), open_mode,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
}

// Always keeps one unconnected pipe instance, so a connecting agent never finds
// the name missing between two accept() calls.
class PipeListener final : public Listener {
public:
    PipeListener(std::wstring name, HANDLE first) : name_(std::move(name)), pending_(first) {}

    std::unique_ptr<Stream> accept() override {
        for (;;) {
            HANDLE h;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_ || pending_ == INVALID_HANDLE_VALUE) return nullptr;
                h = pending_;
            }
            bool connected = ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    CloseHandle(h);
                    return nullptr;
                }
                pending_ = create_instance(name_, false);
            }
            if (connected) return std::make_unique<PipeStream>(h);
            CloseHandle(h);
        }
    }

    // The pipe name disappears when the process exits; nothing to remove.
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

private:
    std::wstring name_;
    std::mutex mutex_;
    HANDLE pending_;
    bool closed_ = false;
};

} // namespace

std::unique_ptr<Listener> listen(const std::string& endpoint, std::string& error) {
    const std::wstring name = win::to_wide(endpoint);
    // WaitNamedPipe fails with ERROR_FILE_NOT_FOUND only when no server has the name.
    // FILE_FLAG_FIRST_PIPE_INSTANCE below also catches a race on Windows (Wine ignores it).
    if (WaitNamedPipeW(name.c_str(), 1) || GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = "another hotpp host is already listening on " + endpoint;
        return nullptr;
    }
    HANDLE first = create_instance(name, true);
    if (first == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED)
            error = "another hotpp host is already listening on " + endpoint;
        else
            error = last_error_text("cannot create pipe " + endpoint);
        return nullptr;
    }
    return std::make_unique<PipeListener>(name, first);
}

int run_child(const std::vector<std::string>& args, std::string& error) {
    std::wstring cmd = win::command_line(args);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError})
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        error = last_error_text("cannot start " + args[0]);
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}

bool set_env(const char* name, const std::string& value) {
    return SetEnvironmentVariableW(win::to_wide(name).c_str(), win::to_wide(value).c_str()) != 0;
}

uint32_t current_pid() { return GetCurrentProcessId(); }

void init_console() { SetConsoleOutputCP(CP_UTF8); }

} // namespace hotpp::host
