// OS layer of the agent on Windows: modules (PE images), patch regions, named-pipe
// IPC, processes and threads.
#include "agent/platform.h"
#include "agent/region_select.h"

#include "common/win_util.h"

#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hotpp::agent {

using win::narrow;
using win::widen;

// --- process -------------------------------------------------------------------

const proto::Platform kPlatform = proto::Platform::Windows;

uint32_t current_pid() { return uint32_t(GetCurrentProcessId()); }

namespace {
std::string module_path(HMODULE m) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(m, buf.data(), DWORD(buf.size()));
    return narrow(buf.data(), n);
}
} // namespace

std::string executable_path() { return module_path(nullptr); }

std::string get_env(const char* name) {
    const std::wstring w = widen(name);
    const DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(w.c_str(), v.data(), n);
    return narrow(v.data(), got);
}

bool set_env(const char* name, const std::string& value) {
    // _putenv_s updates the C runtime's copy (getenv) and the process environment.
    return _putenv_s(name, value.c_str()) == 0;
}

bool file_readable(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string current_directory() {
    const DWORD n = GetCurrentDirectoryW(0, nullptr);
    if (!n) return {};
    std::wstring buf(n, L'\0');
    const DWORD got = GetCurrentDirectoryW(n, buf.data());
    return narrow(buf.data(), got);
}

__declspec(noinline) std::vector<uint64_t> capture_stack(int skip, int max_frames) {
    void* stack[128];
    // Frame 0 is capture_stack itself, frame 1 its caller.
    const USHORT n = RtlCaptureStackBackTrace(DWORD(2 + skip), DWORD(std::min(max_frames, 128)), stack, nullptr);
    std::vector<uint64_t> frames;
    for (USHORT i = 0; i < n; ++i) frames.push_back(uint64_t(reinterpret_cast<uintptr_t>(stack[i])));
    return frames;
}

uint64_t page_size() {
    static const uint64_t size = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return uint64_t(si.dwPageSize);
    }();
    return size;
}

// --- modules -------------------------------------------------------------------

namespace {

// The CodeView record of an image as mapped: {GUID, age}, the identity its PDB
// carries too. Empty if the image has none.
std::vector<uint8_t> codeview_id(const uint8_t* base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return {};
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) return {};
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (DWORD i = 0; i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        const IMAGE_DEBUG_DIRECTORY& e = entries[i];
        if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || e.SizeOfData < 24 || !e.AddressOfRawData) continue;
        const uint8_t* cv = base + e.AddressOfRawData;
        if (std::memcmp(cv, "RSDS", 4) != 0) continue;
        return std::vector<uint8_t>(cv + 4, cv + 24); // GUID (16) + age (4)
    }
    return {};
}

proto::ModuleRecord describe(HMODULE m) {
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info);
    proto::ModuleRecord r;
    r.base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
    r.start = r.base;
    r.size = info.SizeOfImage;
    r.path = module_path(m);
    r.build_id = codeview_id(static_cast<const uint8_t*>(info.lpBaseOfDll));
    return r;
}

std::vector<HMODULE> loaded_modules() {
    std::vector<HMODULE> mods(256);
    DWORD needed = 0;
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed)) return {};
        if (needed <= mods.size() * sizeof(HMODULE)) break;
        mods.resize(needed / sizeof(HMODULE));
    }
    mods.resize(needed / sizeof(HMODULE));
    return mods;
}

} // namespace

std::vector<proto::ModuleRecord> enumerate_modules() {
    std::vector<proto::ModuleRecord> out;
    HMODULE exe = GetModuleHandleW(nullptr);
    out.push_back(describe(exe));
    for (HMODULE m : loaded_modules())
        if (m != exe) out.push_back(describe(m));
    return out;
}

void module_counters(uint64_t& adds, uint64_t& subs) {
    // No loader counters: the count and a checksum of the bases change whenever the
    // module list does.
    const std::vector<HMODULE> mods = loaded_modules();
    adds = mods.size();
    subs = 0;
    for (HMODULE m : mods) subs = subs * 1099511628211ull ^ reinterpret_cast<uint64_t>(m);
}

namespace {

// Unreserved ranges of the address space, from VirtualQuery.
std::vector<AddressRange> free_ranges() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<AddressRange> out;
    auto* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    auto* top = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (p < top && VirtualQuery(p, &mbi, sizeof mbi)) {
        const uint64_t start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        const uint64_t end = start + mbi.RegionSize;
        if (mbi.State == MEM_FREE) out.push_back({start, end});
        p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return out;
}

} // namespace

bool reserve_near(const proto::ModuleRecord& module, uint64_t size, proto::RegionRecord& out) {
    constexpr uint64_t kAlignment = 64 * 1024; // allocation granularity
    // Only above the module: every RVA a patch contains is relative to the module's
    // image base, and RVAs are unsigned (docs/DESIGN.md §13, Windows spike).
    for (uint64_t addr : near_region_candidates(free_ranges(), module.start, module.start + module.size, size, kAlignment, 0)) {
        if (addr < module.start + module.size) continue;
        void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) continue;
        if (reinterpret_cast<uint64_t>(p) != addr) {
            VirtualFree(p, 0, MEM_RELEASE);
            continue;
        }
        out.base = addr;
        out.size = size;
        return true;
    }
    return false;
}

void release_region(const proto::RegionRecord& region) { VirtualFree(reinterpret_cast<void*>(region.base), 0, MEM_RELEASE); }

bool protect(uint64_t addr, uint64_t size, uint8_t prot) {
    const uint64_t page = page_size();
    const uint64_t start = addr & ~(page - 1);
    const uint64_t end = (addr + size + page - 1) & ~(page - 1);
    // Patch region pages are only reserved until first used.
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t p = start; p < end;) {
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) return false;
        const uint64_t next = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (mbi.State == MEM_RESERVE && !VirtualAlloc(reinterpret_cast<void*>(p), next - p, MEM_COMMIT, PAGE_READWRITE))
            return false;
        p = next;
    }
    DWORD flags = PAGE_NOACCESS;
    const bool r = prot & proto::kProtRead, w = prot & proto::kProtWrite, x = prot & proto::kProtExec;
    if (x) flags = w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    else if (w) flags = PAGE_READWRITE;
    else if (r) flags = PAGE_READONLY;
    DWORD old;
    return VirtualProtect(reinterpret_cast<void*>(start), end - start, flags, &old) != 0;
}

void flush_icache(uint64_t addr, uint64_t size) {
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
}

uint64_t resolve_symbol(const std::string& name) {
    // Exports of every loaded module, the executable first.
    HMODULE exe = GetModuleHandleW(nullptr);
    if (FARPROC f = GetProcAddress(exe, name.c_str())) return reinterpret_cast<uint64_t>(f);
    for (HMODULE m : loaded_modules())
        if (m != exe)
            if (FARPROC f = GetProcAddress(m, name.c_str())) return reinterpret_cast<uint64_t>(f);
    return 0;
}

void register_debug_object(const std::vector<uint8_t>&) {
    // No debugger interface for in-memory code on Windows yet.
}

// --- IPC: named pipe -------------------------------------------------------------

struct Connection {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE read_event = nullptr, write_event = nullptr;
    volatile LONG closed = 0;
};

Connection* ipc_connect(const std::string& endpoint) {
    const std::wstring name = widen(endpoint);
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            auto* c = new Connection;
            c->pipe = h;
            c->read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            c->write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            return c;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) return nullptr;
        WaitNamedPipeW(name.c_str(), 100);
    }
    return nullptr;
}

namespace {
bool pipe_io(Connection* c, bool reading, char* buf, DWORD size, DWORD& done) {
    OVERLAPPED ov{};
    ov.hEvent = reading ? c->read_event : c->write_event;
    ResetEvent(ov.hEvent);
    const BOOL ok = reading ? ReadFile(c->pipe, buf, size, nullptr, &ov) : WriteFile(c->pipe, buf, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    return GetOverlappedResult(c->pipe, &ov, &done, TRUE) != 0;
}
} // namespace

bool ipc_send(Connection* c, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        if (c->closed) return false;
        DWORD n = 0;
        if (!pipe_io(c, false, const_cast<char*>(p), DWORD(std::min<size_t>(size, 1u << 20)), n) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

bool ipc_recv(Connection* c, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        if (c->closed) return false;
        DWORD n = 0;
        if (!pipe_io(c, true, p, DWORD(std::min<size_t>(size, 1u << 20)), n) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

void ipc_shutdown(Connection* c) {
    InterlockedExchange(&c->closed, 1);
    CancelIoEx(c->pipe, nullptr);
}

bool ipc_readable(Connection* c, int ms) {
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(ms);
    for (;;) {
        DWORD avail = 0;
        if (c->closed || !PeekNamedPipe(c->pipe, nullptr, 0, nullptr, &avail, nullptr) || avail) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(10);
    }
}

void ipc_close(Connection* c) {
    CloseHandle(c->pipe);
    CloseHandle(c->read_event);
    CloseHandle(c->write_event);
    delete c;
}

// --- processes and threads ---------------------------------------------------------

std::vector<std::string> command_line() {
    std::vector<std::string> out;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out.push_back(narrow(argv[i], wcslen(argv[i])));
    LocalFree(argv);
    return out;
}

std::string exec_self(const std::string&, const std::vector<std::string>&) {
    return "hot restart is not supported on Windows yet";
}

bool spawn_detached(const std::string& program, const std::vector<std::string>& args) {
    std::vector<std::string> argv{program};
    argv.insert(argv.end(), args.begin(), args.end());
    std::wstring cmd = win::command_line(argv);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                        nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct Thread {
    HANDLE handle;
    void (*fn)(void*);
    void* arg;
};

namespace {
DWORD WINAPI thread_trampoline(void* p) {
    auto* t = static_cast<Thread*>(p);
    t->fn(t->arg);
    return 0;
}
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
} // namespace

Thread* thread_start(void (*fn)(void*), void* arg) {
    auto* t = new Thread{nullptr, fn, arg};
    t->handle = CreateThread(nullptr, 0, thread_trampoline, t, 0, nullptr);
    if (!t->handle) {
        delete t;
        return nullptr;
    }
    // Windows 10 1607 and later.
    if (auto set = reinterpret_cast<SetThreadDescriptionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
        set(t->handle, L"hotpp-agent");
    return t;
}

void thread_join(Thread* t) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    delete t;
}

void sleep_ms(unsigned ms) { Sleep(ms); }

} // namespace hotpp::agent
