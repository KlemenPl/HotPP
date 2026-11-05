// Pausing the other threads of the process while patch sites are written, and the
// runtime hooks a patch needs (unwind registration, core serialization).
//
// Threads are stopped in-process: each gets a real-time signal whose handler
// records where the thread was and waits on a futex until the patcher releases it.
#include "agent/platform.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

#include <dirent.h>
#include <linux/futex.h>
#include <linux/membarrier.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

// libgcc's unwinder; weak so the agent links even where it's missing.
extern "C" void __register_frame(void*) __attribute__((weak));

namespace hotpp::agent {

namespace {

constexpr int kMaxThreads = 4096;

struct PauseState {
    std::atomic<uint32_t> generation{0}; // bumped to release paused threads
    std::atomic<int> arrived{0};
    std::atomic<int> departed{0};
    std::atomic<int> next_slot{0};
    std::atomic<uint64_t> rip[kMaxThreads];
};

PauseState g_pause;
std::atomic<pid_t> g_agent_tid{0};
int g_signal = 0;
int g_paused = 0; // threads currently held in the handler
bool g_membarrier = false;

pid_t gettid_raw() { return pid_t(syscall(SYS_gettid)); }

long futex(std::atomic<uint32_t>* addr, int op, uint32_t val) {
    return syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), op, val, nullptr, nullptr, 0);
}

void pause_handler(int, siginfo_t*, void* context) {
    const int saved_errno = errno;
    const uint32_t gen = g_pause.generation.load(std::memory_order_acquire);
    const auto* uc = static_cast<const ucontext_t*>(context);
    const int slot = g_pause.next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot < kMaxThreads)
        g_pause.rip[slot].store(uint64_t(uc->uc_mcontext.gregs[REG_RIP]), std::memory_order_relaxed);
    g_pause.arrived.fetch_add(1, std::memory_order_release);
    while (g_pause.generation.load(std::memory_order_acquire) == gen) futex(&g_pause.generation, FUTEX_WAIT_PRIVATE, gen);
    g_pause.departed.fetch_add(1, std::memory_order_release);
    errno = saved_errno;
}

// A real-time signal nobody else uses, with our handler installed.
bool install_handler(std::string& error) {
    if (g_signal) return true;
    for (int sig = SIGRTMIN + 4; sig <= SIGRTMAX - 2; ++sig) {
        struct sigaction old{};
        if (sigaction(sig, nullptr, &old) != 0) continue;
        if ((old.sa_flags & SA_SIGINFO) || old.sa_handler != SIG_DFL) continue;
        struct sigaction sa{};
        sa.sa_sigaction = pause_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigfillset(&sa.sa_mask);
        if (sigaction(sig, &sa, nullptr) != 0) continue;
        g_signal = sig;
        return true;
    }
    error = "no free real-time signal to pause threads with";
    return false;
}

std::set<pid_t> list_threads() {
    std::set<pid_t> tids;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            tids.insert(pid_t(std::atoi(e->d_name)));
        }
        closedir(d);
    }
    return tids;
}

void sleep_us(long us) {
    timespec ts{0, us * 1000};
    nanosleep(&ts, nullptr);
}

} // namespace

bool register_unwind_info(uint64_t eh_frame) {
    if (!__register_frame) return false;
    __register_frame(reinterpret_cast<void*>(eh_frame));
    return true;
}

void mark_agent_thread() { g_agent_tid = gettid_raw(); }

bool pause_other_threads(std::vector<uint64_t>& stopped_at, std::string& error) {
    if (!install_handler(error)) return false;
    if (!g_membarrier)
        g_membarrier = syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) == 0;

    g_pause.arrived = 0;
    g_pause.departed = 0;
    g_pause.next_slot = 0;
    const pid_t pid = getpid();
    const pid_t self = gettid_raw();
    std::set<pid_t> signalled;

    // Re-list until no new threads show up: one may have been starting meanwhile.
    for (int round = 0; round < 16; ++round) {
        std::set<pid_t> fresh;
        for (pid_t t : list_threads())
            if (t != self && t != g_agent_tid && !signalled.count(t)) fresh.insert(t);
        if (fresh.empty()) break;
        for (pid_t t : fresh) {
            if (syscall(SYS_tgkill, pid, t, g_signal) == 0) signalled.insert(t);
            // ESRCH: the thread exited in the meantime.
        }
        const int want = int(signalled.size());
        for (int waited = 0; g_pause.arrived.load(std::memory_order_acquire) < want; waited += 100) {
            if (waited > 2000000) { // 2 s
                g_paused = g_pause.arrived.load();
                resume_other_threads();
                char buf[160];
                std::snprintf(buf, sizeof buf,
                              "%d of %d threads did not stop (do they block real-time signals?)",
                              want - g_paused, want);
                error = buf;
                return false;
            }
            sleep_us(100);
        }
    }
    g_paused = g_pause.arrived.load(std::memory_order_acquire);
    if (g_paused > kMaxThreads) {
        resume_other_threads();
        error = "too many threads to pause";
        return false;
    }
    stopped_at.clear();
    for (int i = 0; i < g_paused; ++i) stopped_at.push_back(g_pause.rip[i].load(std::memory_order_relaxed));
    return true;
}

void resume_other_threads() {
    const int paused = g_paused;
    g_pause.generation.fetch_add(1, std::memory_order_acq_rel);
    futex(&g_pause.generation, FUTEX_WAKE_PRIVATE, uint32_t(INT32_MAX));
    // Wait until every handler has returned, so the next pause starts clean.
    for (int waited = 0; g_pause.departed.load(std::memory_order_acquire) < paused && waited < 2000000; waited += 50)
        sleep_us(50);
    g_paused = 0;
}

void sync_cores() {
    if (g_membarrier) syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0);
}

} // namespace hotpp::agent
