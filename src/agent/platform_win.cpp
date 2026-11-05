#include "agent/platform.h"
#include "agent/region_select.h"
#include "common/win_util.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>

namespace hotpp::agent {

// --- process -------------------------------------------------------------------

namespace {

std::string module_path(HMODULE module) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(module, buf.data(), DWORD(buf.size()));
    return win::to_utf8(buf.data(), int(n));
}

// Free ranges of the address space between lo and hi, from VirtualQuery.
std::vector<AddressRange> free_ranges(uint64_t lo, uint64_t hi) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint64_t addr = std::max(lo, uint64_t(uintptr_t(si.lpMinimumApplicationAddress)));
    const uint64_t top = std::min(hi, uint64_t(uintptr_t(si.lpMaximumApplicationAddress)));
    std::vector<AddressRange> out;
    while (addr < top) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(addr)), &mbi, sizeof mbi)) break;
        const uint64_t base = uint64_t(uintptr_t(mbi.BaseAddress));
        const uint64_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_FREE) out.push_back({std::max(base, addr), end});
        if (end <= addr) break;
        addr = end;
    }
    return out;
}

} // namespace

uint32_t current_pid() { return GetCurrentProcessId(); }

std::string executable_path() { return module_path(nullptr); }

std::string get_env(const char* name) {
    const std::wstring wname = win::to_wide(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    return win::to_utf8(buf.data(), int(n));
}

std::vector<proto::ModuleRecord> enumerate_modules() {
    HANDLE process = GetCurrentProcess();
    HMODULE main_module = GetModuleHandleW(nullptr);
    std::vector<HMODULE> handles(256);
    DWORD needed = 0;
    for (;;) {
        const DWORD bytes = DWORD(handles.size() * sizeof(HMODULE));
        if (!EnumProcessModules(process, handles.data(), bytes, &needed)) return {};
        if (needed <= bytes) break;
        handles.resize(needed / sizeof(HMODULE));
    }
    handles.resize(needed / sizeof(HMODULE));
    std::stable_partition(handles.begin(), handles.end(),
                          [&](HMODULE h) { return h == main_module; });

    std::vector<proto::ModuleRecord> modules;
    for (HMODULE h : handles) {
        MODULEINFO info;
        if (!GetModuleInformation(process, h, &info, sizeof info)) continue;
        proto::ModuleRecord m;
        m.base = m.start = uint64_t(uintptr_t(info.lpBaseOfDll));
        m.size = info.SizeOfImage;
        m.path = module_path(h);
        modules.push_back(std::move(m));
    }
    return modules;
}

bool reserve_near(const proto::ModuleRecord& module, uint64_t size, proto::RegionRecord& out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint64_t end = module.start + module.size;
    const uint64_t lo = end > kRel32Reach ? end - kRel32Reach : 0;
    const uint64_t hi = module.start + kRel32Reach;
    const std::vector<uint64_t> candidates = near_region_candidates(
        free_ranges(lo, hi), module.start, end, size, si.dwAllocationGranularity, 0);
    for (uint64_t addr : candidates) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(addr)), SIZE_T(size), MEM_RESERVE,
                               PAGE_NOACCESS);
        if (!p) continue;
        if (uint64_t(uintptr_t(p)) != addr) {
            VirtualFree(p, 0, MEM_RELEASE);
            continue;
        }
        out.base = addr;
        out.size = size;
        return true;
    }
    return false;
}

void release_region(const proto::RegionRecord& region) {
    VirtualFree(reinterpret_cast<void*>(uintptr_t(region.base)), 0, MEM_RELEASE);
}

uint64_t page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

bool protect(uint64_t addr, uint64_t size, uint8_t prot) {
    DWORD p = PAGE_NOACCESS;
    const bool r = prot & proto::kProtRead, w = prot & proto::kProtWrite, x = prot & proto::kProtExec;
    if (x) p = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    else if (w) p = PAGE_READWRITE;
    else if (r) p = PAGE_READONLY;
    void* base = reinterpret_cast<void*>(uintptr_t(addr));
    // Patch regions are only reserved; commit their pages on first use.
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(base, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE &&
        !VirtualAlloc(base, SIZE_T(size), MEM_COMMIT, p))
        return false;
    DWORD old = 0;
    return VirtualProtect(base, SIZE_T(size), p, &old) != 0;
}

void flush_icache(uint64_t addr, uint64_t size) {
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(addr)), SIZE_T(size));
}

// Windows processes are not patched yet (M2); these keep the agent linking.
bool register_unwind_info(uint64_t) { return false; }
void mark_agent_thread() {}
bool pause_other_threads(std::vector<uint64_t>& stopped_at, std::string&) {
    stopped_at.clear();
    return true;
}
void resume_other_threads() {}
void sync_cores() {}

uint64_t resolve_symbol(const std::string& name) {
    for (const proto::ModuleRecord& m : enumerate_modules()) {
        HMODULE h = reinterpret_cast<HMODULE>(uintptr_t(m.base));
        if (FARPROC p = GetProcAddress(h, name.c_str())) return uint64_t(uintptr_t(p));
    }
    return 0;
}

// --- IPC: named pipe, overlapped so ipc_shutdown can cancel a blocked read ---------

struct Connection {
    HANDLE pipe;
    HANDLE read_event;
    HANDLE write_event;
};

Connection* ipc_connect(const std::string& endpoint) {
    const std::wstring name = win::to_wide(endpoint);
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            HANDLE re = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE we = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!re || !we) {
                if (re) CloseHandle(re);
                if (we) CloseHandle(we);
                CloseHandle(h);
                return nullptr;
            }
            return new Connection{h, re, we};
        }
        if (GetLastError() != ERROR_PIPE_BUSY) return nullptr;
        WaitNamedPipeW(name.c_str(), 100);
    }
    return nullptr;
}

namespace {
bool overlapped_io(Connection* c, bool write, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        OVERLAPPED ov{};
        ov.hEvent = write ? c->write_event : c->read_event;
        ResetEvent(ov.hEvent);
        const DWORD chunk = DWORD(std::min<size_t>(size, size_t(1) << 20));
        BOOL ok = write ? WriteFile(c->pipe, p, chunk, nullptr, &ov)
                        : ReadFile(c->pipe, p, chunk, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        DWORD done = 0;
        if (!GetOverlappedResult(c->pipe, &ov, &done, TRUE) || done == 0) return false;
        p += done;
        size -= done;
    }
    return true;
}
} // namespace

bool ipc_send(Connection* c, const void* data, size_t size) {
    return overlapped_io(c, true, const_cast<void*>(data), size);
}

bool ipc_recv(Connection* c, void* data, size_t size) { return overlapped_io(c, false, data, size); }

void ipc_shutdown(Connection* c) { CancelIoEx(c->pipe, nullptr); }

void ipc_close(Connection* c) {
    CloseHandle(c->pipe);
    CloseHandle(c->read_event);
    CloseHandle(c->write_event);
    delete c;
}

// --- processes and threads ---------------------------------------------------------

bool spawn_detached(const std::string& program, const std::vector<std::string>& args) {
    std::vector<std::string> all{program};
    all.insert(all.end(), args.begin(), args.end());
    std::wstring cmd = win::command_line(all);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Own console window, so the host's output stays visible after the app exits.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
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
} // namespace

Thread* thread_start(void (*fn)(void*), void* arg) {
    auto* t = new Thread{nullptr, fn, arg};
    t->handle = CreateThread(nullptr, 0, thread_trampoline, t, 0, nullptr);
    if (!t->handle) {
        delete t;
        return nullptr;
    }
    return t;
}

void thread_join(Thread* t) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    delete t;
}

void sleep_ms(unsigned ms) { Sleep(ms); }

} // namespace hotpp::agent
