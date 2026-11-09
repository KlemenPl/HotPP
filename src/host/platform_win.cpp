// OS layer of the host on Windows: named pipes, processes (job objects, ConPTY),
// console control events.
#include "host/platform.h"

#include "common/win_util.h"

#include <aclapi.h>
#include <dbghelp.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace hotpp::host {

using win::error_text;
using win::narrow;
using win::widen;

namespace {

HANDLE as_handle(Handle h) { return reinterpret_cast<HANDLE>(h); }
Handle from_handle(HANDLE h) { return h == INVALID_HANDLE_VALUE || !h ? kNoHandle : reinterpret_cast<Handle>(h); }

// ---- users ---------------------------------------------------------------------

// The SID of the user `process` runs as, as a string; empty if unknown.
std::wstring process_user_sid(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> buf(size ? size : 1);
    std::wstring out;
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        wchar_t* str = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) {
            out = str;
            LocalFree(str);
        }
    }
    CloseHandle(token);
    return out;
}

const std::wstring& own_sid() {
    static const std::wstring sid = process_user_sid(GetCurrentProcess());
    return sid;
}

std::wstring pid_user_sid(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    std::wstring sid = process_user_sid(p);
    CloseHandle(p);
    return sid;
}

// ---- named pipes ---------------------------------------------------------------

constexpr DWORD kPipeBuffer = 64 * 1024;

// Overlapped I/O on one handle; another thread can cancel it.
class PipeStream final : public Stream {
public:
    PipeStream(HANDLE h, bool server) : h_(h), server_(server) {
        read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_read_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeStream() override {
        if (server_) DisconnectNamedPipe(h_);
        CloseHandle(h_);
        CloseHandle(read_event_);
        CloseHandle(write_event_);
        CloseHandle(stop_);
        CloseHandle(stop_read_);
    }

    bool send(const void* data, size_t size) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            if (closed_) return false;
            DWORD n = 0;
            if (!io(false, const_cast<char*>(p), DWORD(std::min<size_t>(size, kPipeBuffer)), n) || n == 0) return false;
            p += n;
            size -= n;
        }
        return true;
    }

    bool recv(void* data, size_t size) override {
        std::lock_guard<std::mutex> lock(read_mutex_);
        char* p = static_cast<char*>(data);
        while (size > 0) {
            if (closed_ || read_closed_) return false;
            DWORD n = 0;
            if (!io(true, p, DWORD(std::min<size_t>(size, kPipeBuffer)), n) || n == 0) return false;
            p += n;
            size -= n;
        }
        return true;
    }

    // The stop events end a wait that is under way or starts later; cancelling the
    // I/O alone would miss a read issued just after.
    void shutdown() override {
        closed_ = true;
        SetEvent(stop_);
    }
    void shutdown_read() override {
        read_closed_ = true;
        SetEvent(stop_read_);
    }

    bool peer_is_same_user() const override {
        const std::wstring sid = peer_sid();
        return !sid.empty() && sid == own_sid();
    }
    std::string peer_description() const override {
        ULONG pid = 0;
        if (server_) GetNamedPipeClientProcessId(h_, &pid);
        return "pid " + std::to_string(pid) + ", user " + narrow(peer_sid());
    }

private:
    std::wstring peer_sid() const {
        ULONG pid = 0;
        const BOOL ok = server_ ? GetNamedPipeClientProcessId(h_, &pid) : GetNamedPipeServerProcessId(h_, &pid);
        return ok ? pid_user_sid(pid) : std::wstring();
    }

    bool io(bool reading, char* buf, DWORD size, DWORD& done) {
        OVERLAPPED& ov = reading ? read_ov_ : write_ov_;
        ov = OVERLAPPED{};
        ov.hEvent = reading ? read_event_ : write_event_;
        ResetEvent(ov.hEvent);
        const BOOL ok = reading ? ReadFile(h_, buf, size, nullptr, &ov) : WriteFile(h_, buf, size, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[3] = {ov.hEvent, stop_, stop_read_};
        const DWORD w = WaitForMultipleObjects(reading ? 3 : 2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(h_, &ov);
            GetOverlappedResult(h_, &ov, &done, TRUE); // the I/O must end before `ov` goes away
            return false;
        }
        return GetOverlappedResult(h_, &ov, &done, FALSE) != 0;
    }

    HANDLE h_;
    bool server_;
    HANDLE read_event_, write_event_, stop_, stop_read_;
    OVERLAPPED read_ov_{}, write_ov_{};
    std::mutex read_mutex_, write_mutex_;
    std::atomic<bool> closed_{false}, read_closed_{false};
};

// Only this user, and only local clients.
struct PipeSecurity {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    PipeSecurity() {
        const std::wstring sddl = L"D:P(A;;GA;;;" + own_sid() + L")";
        if (!own_sid().empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
    }
    ~PipeSecurity() {
        if (sd) LocalFree(sd);
    }
};

// Pipe names can be listed on Windows but not under Wine, so hosts also record
// theirs in %LOCALAPPDATA%\hotpp\endpoints (one empty file per pipe).
std::filesystem::path endpoint_directory() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n)) / L"hotpp" / L"endpoints";
}

constexpr const char* kPipeDir = "\\\\.\\pipe\\";

std::filesystem::path endpoint_record(const std::string& endpoint) {
    const std::filesystem::path dir = endpoint_directory();
    if (dir.empty() || endpoint.rfind(kPipeDir, 0) != 0) return {};
    return dir / widen(endpoint.substr(std::strlen(kPipeDir)));
}

// Whether a pipe of that name exists (busy or not).
bool pipe_exists(const std::string& endpoint) {
    if (WaitNamedPipeW(widen(endpoint).c_str(), 1)) return true;
    const DWORD err = GetLastError();
    return err != ERROR_FILE_NOT_FOUND && err != ERROR_BAD_PATHNAME && err != ERROR_INVALID_NAME;
}

HANDLE create_pipe_instance(const std::wstring& name, bool first) {
    PipeSecurity sec;
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0, &sec.sa);
}

class PipeListener final : public Listener {
public:
    PipeListener(std::wstring name, HANDLE first, std::filesystem::path record)
        : name_(std::move(name)), next_(first), record_(std::move(record)) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        connected_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeListener() override {
        close();
        if (next_ != INVALID_HANDLE_VALUE) CloseHandle(next_);
        CloseHandle(stop_);
        CloseHandle(connected_);
    }

    std::unique_ptr<Stream> accept() override {
        for (;;) {
            if (closed_) return nullptr;
            if (next_ == INVALID_HANDLE_VALUE) next_ = create_pipe_instance(name_, false);
            if (next_ == INVALID_HANDLE_VALUE) return nullptr;
            OVERLAPPED ov{};
            ov.hEvent = connected_;
            ResetEvent(connected_);
            bool ok = ConnectNamedPipe(next_, &ov) != 0;
            DWORD err = ok ? ERROR_PIPE_CONNECTED : GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {connected_, stop_};
                const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0) {
                    CancelIoEx(next_, &ov);
                    DWORD ignored;
                    GetOverlappedResult(next_, &ov, &ignored, TRUE);
                    return nullptr;
                }
                DWORD ignored;
                err = GetOverlappedResult(next_, &ov, &ignored, FALSE) ? ERROR_PIPE_CONNECTED : GetLastError();
            }
            if (err == ERROR_PIPE_CONNECTED) {
                HANDLE h = next_;
                next_ = INVALID_HANDLE_VALUE;
                return std::make_unique<PipeStream>(h, true);
            }
            // The client went away before we got to it (ERROR_NO_DATA): use a fresh instance.
            DisconnectNamedPipe(next_);
            CloseHandle(next_);
            next_ = INVALID_HANDLE_VALUE;
        }
    }

    void close() override {
        if (closed_.exchange(true)) return;
        SetEvent(stop_);
        std::error_code ec;
        if (!record_.empty()) std::filesystem::remove(record_, ec);
    }

private:
    std::wstring name_;
    HANDLE next_;
    std::filesystem::path record_;
    HANDLE stop_, connected_;
    std::atomic<bool> closed_{false};
};

// ---- processes -----------------------------------------------------------------

struct Child {
    HANDLE process = nullptr;
    HANDLE job = nullptr;   // new_process_group: the job holding it and what it starts
    HPCON console = nullptr; // spawn_child_pty: closed once the process exits
};

std::mutex g_children_mutex;
std::map<int, Child> g_children;
std::map<int, Child> g_exited; // waited for; their console and job wait for release_child()
std::atomic<int> g_stop_requests{0}; // counts console stop events, for wait_child

// Values the child inherits: duplicates of ours if they aren't inheritable.
struct Inherit {
    std::vector<HANDLE> owned; // duplicates to close after CreateProcess
    HANDLE get(HANDLE h) {
        if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS))
            return nullptr;
        owned.push_back(dup);
        return dup;
    }
    ~Inherit() {
        for (HANDLE h : owned) CloseHandle(h);
    }
};

std::wstring environment_block(const std::vector<std::string>& overrides) {
    auto name_of = [](const std::wstring& e) {
        // "=C:=C:\x" entries name a drive's directory: keep their leading '='.
        const size_t eq = e.find(L'=', 1);
        std::wstring n = e.substr(0, eq);
        for (wchar_t& c : n) c = wchar_t(towupper(c));
        return n;
    };
    std::map<std::wstring, std::wstring> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring e = p;
            vars[name_of(e)] = e;
        }
        FreeEnvironmentStringsW(env);
    }
    for (const std::string& o : overrides) {
        const std::wstring e = widen(o);
        vars[name_of(e)] = e;
    }
    std::wstring block;
    for (const auto& [name, e] : vars) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

// The command line for argv. `cmd.exe /d /s /c <command>` keeps the command as
// written: cmd doesn't follow the usual quoting rules.
std::wstring build_command_line(const std::vector<std::string>& argv) {
    if (argv.size() == 5 && argv[1] == "/d" && argv[2] == "/s" && argv[3] == "/c")
        return win::command_line({argv[0], argv[1], argv[2], argv[3]}) + L" \"" + widen(argv[4]) + L"\"";
    return win::command_line(argv);
}

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

CreatePseudoConsoleFn create_pseudo_console() {
    static const auto fn = reinterpret_cast<CreatePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole")));
    return fn;
}
ClosePseudoConsoleFn close_pseudo_console() {
    static const auto fn = reinterpret_cast<ClosePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole")));
    return fn;
}

// Returns the pid, or -1 (with `error` set). `started` tells success apart from a
// pid that isn't one: Wine reports no usable pid for a native Linux program.
int start(const std::vector<std::string>& args, const SpawnOptions& opt, HPCON console, std::string& error,
          bool* started = nullptr) {
    if (started) *started = false;
    if (args.empty()) {
        error = "no program to start";
        return -1;
    }
    Inherit inherit;
    HANDLE stdio[3] = {nullptr, nullptr, nullptr};
    std::vector<HANDLE> extra;
    // Extra handles keep their value (the child is told it, see handle_argument): made
    // inheritable just for this call. The handle list keeps other spawns from getting them.
    std::vector<HANDLE> made_inheritable;
    for (const auto& [h, n] : opt.handles) {
        if (n >= 0 && n <= 2) {
            stdio[n] = inherit.get(as_handle(h));
        } else {
            DWORD flags = 0;
            if (GetHandleInformation(as_handle(h), &flags) && !(flags & HANDLE_FLAG_INHERIT) &&
                SetHandleInformation(as_handle(h), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                made_inheritable.push_back(as_handle(h));
            extra.push_back(as_handle(h));
        }
    }
    HANDLE null_in = nullptr;
    if (opt.null_stdin && !stdio[0]) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        stdio[0] = null_in == INVALID_HANDLE_VALUE ? nullptr : null_in;
    }
    if (!console) {
        const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        for (int i = 0; i < 3; ++i)
            if (!stdio[i]) stdio[i] = inherit.get(GetStdHandle(ids[i]));
    }

    std::vector<HANDLE> list;
    for (HANDLE h : stdio)
        if (h && std::find(list.begin(), list.end(), h) == list.end()) list.push_back(h);
    for (HANDLE h : extra)
        if (h && std::find(list.begin(), list.end(), h) == list.end()) list.push_back(h);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size);
    if (!list.empty())
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE),
                                  nullptr, nullptr);
    if (console)
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(HPCON), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    if (!console) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = stdio[0];
        si.StartupInfo.hStdOutput = stdio[1];
        si.StartupInfo.hStdError = stdio[2];
    }

    std::wstring cmdline = build_command_line(args);
    std::wstring env = environment_block(opt.env);
    const std::wstring program = widen(opt.program);
    const std::wstring cwd = opt.cwd.empty() ? std::wstring() : opt.cwd.wstring();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    if (opt.new_process_group) flags |= CREATE_NEW_PROCESS_GROUP;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(program.empty() ? nullptr : program.c_str(), cmdline.data(), nullptr, nullptr,
                                   list.empty() ? FALSE : TRUE, flags, env.data(),
                                   cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD err = GetLastError();
    for (HANDLE h : made_inheritable) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    DeleteProcThreadAttributeList(attrs);
    if (null_in && null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        error = error_text(("cannot start " + (opt.program.empty() ? args[0] : opt.program)).c_str(), err);
        return -1;
    }
    if (started) *started = true;
    Child child;
    child.process = pi.hProcess;
    child.console = console;
    if (opt.new_process_group) {
        // A job, so the process and everything it starts can be killed together.
        child.job = CreateJobObjectW(nullptr, nullptr);
        if (child.job && !AssignProcessToJobObject(child.job, pi.hProcess)) {
            CloseHandle(child.job);
            child.job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::lock_guard<std::mutex> lock(g_children_mutex);
    g_children[int(pi.dwProcessId)] = child;
    return int(pi.dwProcessId);
}

HANDLE child_process(int pid) {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it != g_children.end()) return it->second.process;
    // Not started by us (a hot restart's successor, see adopt_child): open it.
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!h) return nullptr;
    g_children[pid].process = h;
    return h;
}

} // namespace

std::unique_ptr<Listener> listen(const std::string& endpoint, std::string& error) {
    if (endpoint.rfind("\\\\.\\pipe\\", 0) != 0) {
        error = "not a pipe name (\\\\.\\pipe\\...): " + endpoint;
        return nullptr;
    }
    const std::wstring name = widen(endpoint);
    HANDLE first = create_pipe_instance(name, true);
    if (first == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        error = err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY
                    ? "another hotpp host is already listening on " + endpoint
                    : error_text(("cannot listen on " + endpoint).c_str(), err);
        return nullptr;
    }
    const std::filesystem::path record = endpoint_record(endpoint);
    if (!record.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(record.parent_path(), ec);
        FILE* f = _wfopen(record.c_str(), L"wb");
        if (f) std::fclose(f);
    }
    return std::make_unique<PipeListener>(name, first, record);
}

std::unique_ptr<Stream> connect(const std::string& endpoint, std::string& error) {
    const std::wstring name = widen(endpoint);
    for (int attempt = 0;; ++attempt) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            auto s = std::make_unique<PipeStream>(h, false);
            // Pipe names are global: refuse one another user created.
            if (!s->peer_is_same_user()) {
                error = endpoint + " belongs to another user";
                return nullptr;
            }
            return s;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY && attempt < 20 && WaitNamedPipeW(name.c_str(), 100)) continue;
        if (err == ERROR_PIPE_BUSY && attempt < 20) continue;
        error = error_text(("cannot connect to " + endpoint).c_str(), err);
        return nullptr;
    }
}

std::vector<std::string> list_endpoints(const std::string& prefix) {
    std::vector<std::string> out;
    const std::string dir = kPipeDir;
    if (prefix.rfind(dir, 0) != 0) return out;
    const std::string base = prefix.substr(dir.size());
    auto consider = [&](const std::string& name) {
        if ((name == base || name.rfind(base + "-", 0) == 0) &&
            std::find(out.begin(), out.end(), dir + name) == out.end())
            out.push_back(dir + name);
    };
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do consider(narrow(fd.cFileName, wcslen(fd.cFileName)));
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    // Records left by hosts that died are removed here.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(endpoint_directory(), ec)) {
        const std::string name = narrow(e.path().filename().wstring());
        if (name != base && name.rfind(base + "-", 0) != 0) continue;
        if (pipe_exists(dir + name)) {
            consider(name);
        } else {
            std::error_code rec;
            std::filesystem::remove(e.path(), rec);
        }
    }
    return out;
}

std::string endpoint_identity(const std::string& endpoint) {
    // A pipe disappears with the process that created it, so its name is enough.
    return pipe_exists(endpoint) ? endpoint : std::string();
}

// ---- events and pipes ----------------------------------------------------------

WakeEvent::WakeEvent() : handle_(from_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))) {}
WakeEvent::~WakeEvent() {
    if (handle_ != kNoHandle) CloseHandle(as_handle(handle_));
}
void WakeEvent::set() const { SetEvent(as_handle(handle_)); }
void WakeEvent::reset() const { ResetEvent(as_handle(handle_)); }
bool WakeEvent::wait(int timeout_ms) const {
    return WaitForSingleObject(as_handle(handle_), timeout_ms < 0 ? INFINITE : DWORD(timeout_ms)) == WAIT_OBJECT_0;
}

long read_some(Handle h, void* data, size_t size, int timeout_ms, const WakeEvent* wake) {
    HANDLE f = as_handle(h);
    if (GetFileType(f) != FILE_TYPE_PIPE) {
        // Files and consoles: a plain blocking read.
        DWORD n = 0;
        if (!ReadFile(f, data, DWORD(std::min<size_t>(size, 1u << 20)), &n, nullptr))
            return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        return long(n);
    }
    // Anonymous pipes have no overlapped I/O: wait for data with PeekNamedPipe.
    const ULONGLONG deadline = timeout_ms < 0 ? 0 : GetTickCount64() + ULONGLONG(timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(f, nullptr, 0, nullptr, &avail, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) return 0;
            // A pipe that can't be peeked (Wine, for a pipe from the Unix side): read,
            // without the timeout.
            DWORD n = 0;
            if (!ReadFile(f, data, DWORD(std::min<size_t>(size, 1u << 20)), &n, nullptr))
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            return long(n);
        }
        if (avail) {
            DWORD n = 0;
            if (!ReadFile(f, data, DWORD(std::min<size_t>({size, size_t(avail), size_t(1u << 20)})), &n, nullptr))
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            return long(n);
        }
        if (timeout_ms >= 0 && GetTickCount64() >= deadline) return -2;
        if (wake ? wake->wait(10) : (Sleep(10), false)) return -2;
    }
}

bool write_all(Handle h, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD n = 0;
        if (!WriteFile(as_handle(h), p, DWORD(std::min<size_t>(size, 1u << 20)), &n, nullptr) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

void close_handle(Handle h) {
    if (h != kNoHandle) CloseHandle(as_handle(h));
}

bool make_pipe(Pipe& pipe, std::string& error) {
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, nullptr, 0)) {
        error = error_text("pipe");
        return false;
    }
    pipe.read = from_handle(r);
    pipe.write = from_handle(w);
    return true;
}

std::string handle_argument(Handle h, int) { return std::to_string(static_cast<unsigned long long>(h)); }

Handle parse_handle(const std::string& argument) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(argument.c_str(), &end, 10);
    if (!end || *end || v == 0) return kNoHandle;
    DWORD flags = 0;
    if (!GetHandleInformation(reinterpret_cast<HANDLE>(uintptr_t(v)), &flags)) return kNoHandle;
    return Handle(v);
}

void keep_private(Handle h) { SetHandleInformation(as_handle(h), HANDLE_FLAG_INHERIT, 0); }

// ---- processes -----------------------------------------------------------------

int spawn_process(const std::vector<std::string>& args, const SpawnOptions& opt, std::string& error) {
    return start(args, opt, nullptr, error);
}

int spawn_child(const std::vector<std::string>& args, std::string& error) { return spawn_process(args, {}, error); }

namespace {
bool under_wine();
} // namespace

int spawn_child_pty(const std::vector<std::string>& args, Handle& output, std::string& error) {
    output = kNoHandle;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, nullptr, 0) || !CreatePipe(&out_r, &out_w, nullptr, 0)) {
        error = error_text("pipe");
        return -1;
    }
    HPCON console = nullptr;
    // Wine's pseudo-console doesn't capture the program's output: pipes there.
    auto create = under_wine() ? nullptr : create_pseudo_console();
    if (create && SUCCEEDED(create(COORD{200, 50}, in_r, out_w, 0, &console))) {
        CloseHandle(in_r);
        CloseHandle(out_w);
        CloseHandle(in_w); // the program reads nothing
        SpawnOptions opt;
        const int pid = start(args, opt, console, error);
        if (pid < 0) {
            close_pseudo_console()(console);
            CloseHandle(out_r);
            return -1;
        }
        output = from_handle(out_r);
        return pid;
    }
    // No pseudo-console (Windows before 10 1809, Wine): plain pipes. The program's C
    // runtime then buffers its output in blocks, not lines.
    CloseHandle(in_r);
    CloseHandle(in_w);
    SpawnOptions opt;
    opt.null_stdin = true;
    opt.handles = {{from_handle(out_w), 1}, {from_handle(out_w), 2}};
    const int pid = start(args, opt, nullptr, error);
    CloseHandle(out_w);
    if (pid < 0) {
        CloseHandle(out_r);
        return -1;
    }
    output = from_handle(out_r);
    return pid;
}

int wait_child(int pid, const std::function<void()>& on_signal) {
    HANDLE h = child_process(pid);
    if (!h) return -1;
    int seen = g_stop_requests.load();
    // WAIT_FAILED: a native program under Wine (see run_capture).
    while (WaitForSingleObject(h, 100) == WAIT_TIMEOUT) {
        const int now = g_stop_requests.load();
        if (now != seen) {
            seen = now;
            if (on_signal) on_signal();
        }
    }
    DWORD code = DWORD(-1);
    GetExitCodeProcess(h, &code);
    {
        std::lock_guard<std::mutex> lock(g_children_mutex);
        g_exited[pid] = g_children[pid];
        g_children.erase(pid);
    }
    return int(code);
}

void adopt_child(int old_pid, int new_pid) {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(old_pid);
    Child* old = it != g_children.end() ? &it->second : nullptr;
    if (!old) {
        auto e = g_exited.find(old_pid);
        if (e == g_exited.end()) return;
        old = &e->second;
    }
    Child& next = g_children[new_pid];
    if (!next.process)
        next.process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, DWORD(new_pid));
    next.console = old->console;
    next.job = old->job;
    old->console = nullptr;
    old->job = nullptr;
}

void release_child(int pid) {
    Child child;
    {
        std::lock_guard<std::mutex> lock(g_children_mutex);
        auto it = g_exited.find(pid);
        if (it == g_exited.end()) return;
        child = it->second;
        g_exited.erase(it);
    }
    // The pseudo-console keeps its output pipe open until it is closed.
    if (child.console) close_pseudo_console()(child.console);
    if (child.job) CloseHandle(child.job);
    if (child.process) CloseHandle(child.process);
}

void stop_process(int pid, bool force) {
    if (!force && GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, DWORD(pid))) return;
    HANDLE h = child_process(pid);
    HANDLE opened = h ? nullptr : OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (h || opened) TerminateProcess(h ? h : opened, 1);
    if (opened) CloseHandle(opened);
}

void kill_process_group(int pid) {
    HANDLE job = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_children_mutex);
        auto it = g_children.find(pid);
        if (it != g_children.end()) job = it->second.job;
    }
    if (job && TerminateJobObject(job, 1)) return;
    stop_process(pid, true);
}

bool process_alive(uint32_t pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

namespace {

bool under_wine() {
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    return wine;
}

// Wine's own export: the Unix path of a Windows path (allocated on the process heap).
std::string unix_path(const std::filesystem::path& p) {
    using Fn = char*(CDECL*)(const wchar_t*);
    static const auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name")));
    if (!fn) return {};
    char* u = fn(std::filesystem::absolute(p).wstring().c_str());
    if (!u) return {};
    std::string out = u;
    HeapFree(GetProcessHeap(), 0, u);
    return out;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string read_whole(const std::filesystem::path& p) {
    std::string out;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

// Under Wine, a Linux program (a cross build's compiler) runs natively, but Wine
// neither redirects its standard handles nor lets its process handle be waited for.
// The shell redirects its output to a file and writes its exit status to another.
int run_native_under_wine(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& output,
                          std::string& error) {
    static std::atomic<int> counter{0};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    // Unique: Wine hands out the same pids again and again.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const std::filesystem::path base = std::filesystem::path(tmp) / ("hotpp-run-" + std::to_string(GetCurrentProcessId()) + "-" +
                                                                     std::to_string(now.QuadPart) + "-" +
                                                                     std::to_string(counter++));
    const std::filesystem::path out_file = base.wstring() + L".out", status_file = base.wstring() + L".status";
    std::error_code stale;
    std::filesystem::remove(status_file, stale);
    const std::string out_u = unix_path(out_file), status_u = unix_path(status_file);
    const std::string cwd_u = cwd.empty() ? std::string() : unix_path(cwd);
    if (out_u.empty() || status_u.empty()) {
        error = "can't map paths for " + args[0] + " under Wine";
        return -1;
    }
    std::string script = cwd_u.empty() ? std::string() : "cd " + shell_quote(cwd_u) + " && ";
    script += "\"$@\" >" + shell_quote(out_u) + " 2>&1; echo $? >" + shell_quote(status_u + ".tmp") + "; mv " +
              shell_quote(status_u + ".tmp") + " " + shell_quote(status_u);
    std::vector<std::string> argv = {"/bin/sh", "-c", script, "sh"};
    argv.insert(argv.end(), args.begin(), args.end());
    SpawnOptions opt;
    opt.null_stdin = true;
    bool started = false;
    const int pid = start(argv, opt, nullptr, error, &started);
    if (!started) return -1;
    {
        // Its handle (and pid) are of no use (see above).
        std::lock_guard<std::mutex> lock(g_children_mutex);
        auto it = g_children.find(pid);
        if (it != g_children.end()) {
            CloseHandle(it->second.process);
            if (it->second.job) CloseHandle(it->second.job);
            g_children.erase(it);
        }
    }
    std::error_code ec;
    for (int waited = 0; !std::filesystem::exists(status_file, ec); waited += 10) {
        if (waited > 30 * 60 * 1000) {
            error = args[0] + " didn't finish";
            return -1;
        }
        Sleep(10);
    }
    output = read_whole(out_file);
    const int code = std::atoi(read_whole(status_file).c_str());
    std::filesystem::remove(out_file, ec);
    std::filesystem::remove(status_file, ec);
    return code;
}

} // namespace

int run_capture(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& output,
                std::string& error) {
    if (under_wine() && !args.empty() && !args[0].empty() && args[0][0] == '/')
        return run_native_under_wine(args, cwd, output, error);
    Pipe p;
    if (!make_pipe(p, error)) return -1;
    SpawnOptions opt;
    opt.cwd = cwd;
    opt.null_stdin = true;
    opt.handles = {{p.write, 1}, {p.write, 2}};
    const int pid = spawn_process(args, opt, error);
    close_handle(p.write);
    if (pid < 0) {
        close_handle(p.read);
        return -1;
    }
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(as_handle(p.read), buf, sizeof buf, &n, nullptr) || n == 0) break;
        output.append(buf, n);
    }
    close_handle(p.read);
    const int code = wait_child(pid, {});
    release_child(pid);
    return code;
}

std::vector<std::string> shell_command(const std::string& command) {
    // Under Wine the build tools are Linux ones: run them with the Linux shell.
    if (under_wine()) return {"/bin/sh", "-c", command};
    return {"cmd.exe", "/d", "/s", "/c", command};
}

const char* shell_description() { return "cmd.exe /c"; }

bool set_env(const char* name, const std::string& value) {
    // _putenv_s updates the C runtime's copy (getenv) and the process environment.
    return _putenv_s(name, value.c_str()) == 0;
}

uint32_t current_pid() { return uint32_t(GetCurrentProcessId()); }

std::filesystem::path self_executable() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return n ? std::filesystem::path(buf) : std::filesystem::path("hotpp.exe");
}

bool stdout_is_terminal() {
    static const bool terminal = [] {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (!GetConsoleMode(out, &mode)) return false;
        // Colors are ANSI sequences: let the console interpret them.
        return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }();
    return terminal;
}

std::string demangle(const std::string& name) {
    if (name.empty() || name[0] != '?') return name;
    char buf[2048];
    static std::mutex dbghelp; // DbgHelp isn't thread-safe
    std::lock_guard<std::mutex> lock(dbghelp);
    const DWORD n = UnDecorateSymbolName(name.c_str(), buf, sizeof buf,
                                         UNDNAME_NO_MS_KEYWORDS | UNDNAME_NO_ACCESS_SPECIFIERS | UNDNAME_NO_FUNCTION_RETURNS |
                                             UNDNAME_NO_ALLOCATION_MODEL | UNDNAME_NO_ALLOCATION_LANGUAGE |
                                             UNDNAME_NO_MEMBER_TYPE | UNDNAME_NO_THROW_SIGNATURES);
    if (!n) return name;
    std::string out(buf, n);
    // "f(void)" reads as "f()" elsewhere.
    for (size_t at; (at = out.find("(void)")) != std::string::npos;) out.replace(at, 6, "()");
    return out;
}

// ---- console events and stdio --------------------------------------------------

namespace {
void (*g_on_stop)(StopRequest) = nullptr;

BOOL WINAPI console_handler(DWORD event) {
    if (!g_on_stop) return FALSE;
    g_stop_requests.fetch_add(1);
    g_on_stop(event == CTRL_C_EVENT ? StopRequest::Interrupt : StopRequest::Terminate);
    return TRUE;
}
} // namespace

void install_signal_handlers(void (*on_stop)(StopRequest), void (*)()) {
    g_on_stop = on_stop;
    SetConsoleCtrlHandler(console_handler, TRUE);
}

const char* restart_request_hint() { return "run `hotpp restart`"; }

Handle take_stdout(std::string& error) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE dup = nullptr;
    if (!out || out == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = error_text("stdout");
        return kNoHandle;
    }
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    _dup2(_fileno(stderr), _fileno(stdout));
    return from_handle(dup);
}

Handle stdin_handle() { return from_handle(GetStdHandle(STD_INPUT_HANDLE)); }

} // namespace hotpp::host
