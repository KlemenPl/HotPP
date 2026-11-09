// Pausing the other threads of the process while patch sites are written, and the
// runtime hooks a patch needs (unwind registration, core serialization), on Windows.
//
// Threads are suspended with SuspendThread. A suspended thread may hold any lock,
// the process heap's included, so nothing here allocates from the heap while a
// thread is suspended: thread lists come from NtQuerySystemInformation into memory
// reserved up front.
#include "agent/platform.h"

#include "common/win_util.h"

#include <winternl.h>

#include <atomic>
#include <cstdio>

namespace hotpp::agent {

namespace {

constexpr int kMaxThreads = 4096;
constexpr size_t kSnapshotBytes = 4u << 20;

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);

// The documented SYSTEM_PROCESS_INFORMATION is truncated; these are the fields
// needed, laid out as Windows (and Wine) return them.
struct ThreadInfo {
    LARGE_INTEGER times[3];
    ULONG wait_time;
    void* start_address;
    CLIENT_ID client_id;
    LONG priority, base_priority;
    ULONG context_switches, state, wait_reason;
};
struct ProcessInfo {
    ULONG next_entry_offset;
    ULONG number_of_threads;
    LARGE_INTEGER reserved[6];
    UNICODE_STRING image_name;
    LONG base_priority;
    HANDLE unique_process_id;
    HANDLE inherited_from;
    ULONG handle_count, session_id;
    ULONG_PTR unique_process_key;
    SIZE_T vm[11];
    LARGE_INTEGER io[6];
    ThreadInfo threads[1];
};

std::atomic<DWORD> g_agent_tid{0};

struct Suspended {
    DWORD tid;
    HANDLE handle;
};
Suspended g_suspended[kMaxThreads];
int g_count = 0;
uint8_t* g_snapshot = nullptr; // kSnapshotBytes, VirtualAlloc'd once

NtQuerySystemInformationFn query() {
    static const auto fn = reinterpret_cast<NtQuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    return fn;
}

bool already(DWORD tid) {
    for (int i = 0; i < g_count; ++i)
        if (g_suspended[i].tid == tid) return true;
    return false;
}

// Suspends threads of this process not suspended yet. Returns how many it added,
// or -1 if the threads can't be listed.
int suspend_new(DWORD self) {
    constexpr ULONG kSystemProcessInformation = 5;
    ULONG len = 0;
    if (query()(kSystemProcessInformation, g_snapshot, ULONG(kSnapshotBytes), &len) < 0) return -1;
    const DWORD pid = GetCurrentProcessId();
    int added = 0;
    for (auto* p = reinterpret_cast<ProcessInfo*>(g_snapshot);;) {
        if (DWORD(reinterpret_cast<uintptr_t>(p->unique_process_id)) == pid) {
            for (ULONG i = 0; i < p->number_of_threads; ++i) {
                const DWORD tid = DWORD(reinterpret_cast<uintptr_t>(p->threads[i].client_id.UniqueThread));
                if (tid == self || tid == g_agent_tid || already(tid) || g_count >= kMaxThreads) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                if (!h) continue; // exited meanwhile
                if (SuspendThread(h) == DWORD(-1)) {
                    CloseHandle(h);
                    continue;
                }
                g_suspended[g_count++] = {tid, h};
                ++added;
            }
            break;
        }
        if (!p->next_entry_offset) break;
        p = reinterpret_cast<ProcessInfo*>(reinterpret_cast<uint8_t*>(p) + p->next_entry_offset);
    }
    return added;
}

} // namespace

bool register_unwind_info(uint64_t) { return false; } // ELF only

bool register_function_table(uint64_t table, uint64_t count, uint64_t image_base) {
    return RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(table), DWORD(count), image_base) != 0;
}

void mark_agent_thread() { g_agent_tid = GetCurrentThreadId(); }

bool pause_other_threads(std::vector<uint64_t>& stopped_at, std::string& error) {
    if (!query()) {
        error = "NtQuerySystemInformation is unavailable";
        return false;
    }
    if (!g_snapshot) {
        g_snapshot = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSnapshotBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_snapshot) {
            error = win::error_text("VirtualAlloc");
            return false;
        }
    }
    stopped_at.clear();
    stopped_at.reserve(kMaxThreads); // no allocation while threads are suspended
    g_count = 0;
    const DWORD self = GetCurrentThreadId();
    // Re-list until no new threads show up: one may have been starting meanwhile.
    for (int round = 0; round < 16; ++round) {
        const int added = suspend_new(self);
        if (added < 0) {
            resume_other_threads();
            error = "can't list the process's threads";
            return false;
        }
        if (added == 0) break;
    }
    if (g_count >= kMaxThreads) {
        resume_other_threads();
        error = "too many threads to pause";
        return false;
    }
    for (int i = 0; i < g_count; ++i) {
        // GetThreadContext also waits until the suspension has taken effect.
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(g_suspended[i].handle, &ctx)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "can't read thread %lu's registers", g_suspended[i].tid);
            resume_other_threads();
            error = buf;
            return false;
        }
        stopped_at.push_back(ctx.Rip);
    }
    return true;
}

void resume_other_threads() {
    for (int i = 0; i < g_count; ++i) {
        ResumeThread(g_suspended[i].handle);
        CloseHandle(g_suspended[i].handle);
    }
    g_count = 0;
}

void sync_cores() {
    // An interrupt to every core running the process, like membarrier on Linux.
    FlushProcessWriteBuffers();
}

} // namespace hotpp::agent
