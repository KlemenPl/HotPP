#include "agent/platform.h"
#include "agent/region_select.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

namespace hotpp::agent {

// --- process -------------------------------------------------------------------

const proto::Platform kPlatform = proto::Platform::Linux;

uint32_t current_pid() { return uint32_t(getpid()); }

std::string executable_path() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool set_env(const char* name, const std::string& value) { return setenv(name, value.c_str(), 1) == 0; }

bool file_readable(const std::string& path) { return access(path.c_str(), R_OK) == 0; }

std::string current_directory() {
    char cwd[PATH_MAX];
    return getcwd(cwd, sizeof cwd) ? std::string(cwd) : std::string();
}

__attribute__((noinline)) std::vector<uint64_t> capture_stack(int skip, int max_frames) {
    void* stack[128];
    const int depth = backtrace(stack, std::min(max_frames + skip + 2, 128));
    std::vector<uint64_t> frames;
    // [0] is in capture_stack, [1] in its caller.
    for (int i = 2 + skip; i < depth; ++i) frames.push_back(uint64_t(reinterpret_cast<uintptr_t>(stack[i])));
    return frames;
}

uint64_t page_size() {
    static const uint64_t size = uint64_t(sysconf(_SC_PAGESIZE));
    return size;
}

namespace {

// NT_GNU_BUILD_ID from the module's PT_NOTE segments, as mapped in memory.
std::vector<uint8_t> build_id(const dl_phdr_info* info) {
    constexpr uint32_t kNtGnuBuildId = 3;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        const uint8_t* end = p + ph.p_memsz;
        while (p + 12 <= end) {
            uint32_t namesz, descsz, type;
            std::memcpy(&namesz, p, 4);
            std::memcpy(&descsz, p + 4, 4);
            std::memcpy(&type, p + 8, 4);
            const uint8_t* name = p + 12;
            const uint8_t* desc = name + ((namesz + 3) & ~3u);
            const uint8_t* next = desc + ((descsz + 3) & ~3u);
            if (next > end) break;
            if (type == kNtGnuBuildId && namesz == 4 && std::memcmp(name, "GNU", 4) == 0)
                return std::vector<uint8_t>(desc, desc + descsz);
            p = next;
        }
    }
    return {};
}

int collect_module(dl_phdr_info* info, size_t, void* data) {
    auto& modules = *static_cast<std::vector<proto::ModuleRecord>*>(data);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        const uint64_t start = info->dlpi_addr + ph.p_vaddr;
        lo = std::min(lo, start);
        hi = std::max(hi, start + ph.p_memsz);
    }
    if (hi == 0) return 0;
    const uint64_t page = page_size();
    proto::ModuleRecord m;
    m.base = info->dlpi_addr;
    m.start = lo & ~(page - 1);
    m.size = ((hi + page - 1) & ~(page - 1)) - m.start;
    // dl_iterate_phdr visits the main program first, with an empty name.
    m.path = modules.empty() ? executable_path() : (info->dlpi_name ? info->dlpi_name : "");
    m.build_id = build_id(info);
    // TLS: dlpi_tls_data is this thread's copy of the module's block. For modules
    // loaded at startup it's in static TLS, at a fixed offset from the thread pointer.
    m.tls_module_id = info->dlpi_tls_modid;
    if (info->dlpi_tls_data) {
        uint64_t tp;
        __asm__("mov %%fs:0, %0" : "=r"(tp));
        m.tls_offset = int64_t(reinterpret_cast<uint64_t>(info->dlpi_tls_data) - tp);
        m.tls_static = true;
    }
    modules.push_back(std::move(m));
    return 0;
}

// Unmapped ranges of the address space, from /proc/self/maps.
std::vector<AddressRange> free_ranges() {
    constexpr uint64_t kLowest = 0x10000;            // above vm.mmap_min_addr
    constexpr uint64_t kUserTop = 0x7FFFFFFFF000ull; // 47-bit user space
    std::vector<AddressRange> out;
    FILE* f = std::fopen("/proc/self/maps", "re");
    if (!f) return out;
    char* line = nullptr;
    size_t cap = 0;
    uint64_t prev_end = kLowest;
    while (getline(&line, &cap, f) > 0) {
        unsigned long long start = 0, end = 0;
        if (std::sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
        if (start > prev_end) out.push_back({prev_end, std::min<uint64_t>(start, kUserTop)});
        prev_end = std::max<uint64_t>(prev_end, end);
    }
    if (prev_end < kUserTop) out.push_back({prev_end, kUserTop});
    std::free(line);
    std::fclose(f);
    return out;
}

} // namespace

std::vector<proto::ModuleRecord> enumerate_modules() {
    std::vector<proto::ModuleRecord> modules;
    dl_iterate_phdr(collect_module, &modules);
    return modules;
}

void module_counters(uint64_t& adds, uint64_t& subs) {
    struct Counters {
        uint64_t adds = 0, subs = 0;
    } c;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t size, void* data) {
            auto* out = static_cast<Counters*>(data);
            if (size >= offsetof(dl_phdr_info, dlpi_subs) + sizeof(info->dlpi_subs)) {
                out->adds = info->dlpi_adds;
                out->subs = info->dlpi_subs;
            }
            return 1; // the counters are the same in every entry
        },
        &c);
    adds = c.adds;
    subs = c.subs;
}

bool reserve_near(const proto::ModuleRecord& module, uint64_t size, proto::RegionRecord& out) {
    constexpr uint64_t kAlignment = 64 * 1024;
    constexpr uint64_t kAboveGuard = uint64_t(64) << 20; // room for the brk heap
    const std::vector<uint64_t> candidates = near_region_candidates(
        free_ranges(), module.start, module.start + module.size, size, kAlignment, kAboveGuard);
    for (uint64_t addr : candidates) {
        void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (reinterpret_cast<uint64_t>(p) != addr) {
            munmap(p, size); // kernel < 4.17 treated the address as a hint
            continue;
        }
        // Shows up as [anon:hotpp-patch-region] in /proc/<pid>/maps where supported.
        prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<unsigned long>(p),
              static_cast<unsigned long>(size),
              reinterpret_cast<unsigned long>("hotpp-patch-region"));
        out.base = addr;
        out.size = size;
        return true;
    }
    return false;
}

void release_region(const proto::RegionRecord& region) {
    munmap(reinterpret_cast<void*>(region.base), region.size);
}

bool protect(uint64_t addr, uint64_t size, uint8_t prot) {
    const uint64_t page = page_size();
    const uint64_t start = addr & ~(page - 1);
    const uint64_t end = (addr + size + page - 1) & ~(page - 1);
    int p = PROT_NONE;
    if (prot & proto::kProtRead) p |= PROT_READ;
    if (prot & proto::kProtWrite) p |= PROT_WRITE;
    if (prot & proto::kProtExec) p |= PROT_EXEC;
    return mprotect(reinterpret_cast<void*>(start), end - start, p) == 0;
}

void flush_icache(uint64_t addr, uint64_t size) {
    __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr + size));
}

uint64_t resolve_symbol(const std::string& name) {
    return reinterpret_cast<uint64_t>(dlsym(RTLD_DEFAULT, name.c_str()));
}

// --- IPC: Unix domain socket ---------------------------------------------------------

struct Connection {
    int fd;
};

Connection* ipc_connect(const std::string& endpoint) {
    sockaddr_un addr{};
    if (endpoint.empty() || endpoint.size() >= sizeof addr.sun_path) return nullptr;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.data(), endpoint.size());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return nullptr;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return nullptr;
    }
    return new Connection{fd};
}

bool ipc_send(Connection* c, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t n = send(c->fd, p, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= size_t(n);
    }
    return true;
}

bool ipc_recv(Connection* c, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        ssize_t n = recv(c->fd, p, size, 0);
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

void ipc_shutdown(Connection* c) { shutdown(c->fd, SHUT_RDWR); }

bool ipc_readable(Connection* c, int ms) {
    pollfd p{c->fd, POLLIN, 0};
    return ::poll(&p, 1, ms) != 0; // ready, closed, or an error: let recv find out
}

void ipc_close(Connection* c) {
    close(c->fd);
    delete c;
}

// --- processes and threads ---------------------------------------------------------

std::vector<std::string> command_line() {
    std::vector<std::string> out;
    FILE* f = std::fopen("/proc/self/cmdline", "re");
    if (!f) return out;
    std::string cur;
    for (int c; (c = std::fgetc(f)) != EOF;) {
        if (c == 0) {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += char(c);
        }
    }
    std::fclose(f);
    return out;
}

std::string exec_self(const std::string& path, const std::vector<std::string>& args, void (*announce)(uint32_t)) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    announce(current_pid());
    // Descriptors the app didn't mark close-on-exec stay open, as with any exec.
    execv(path.c_str(), argv.data());
    return std::string("exec ") + path + ": " + std::strerror(errno);
}

bool spawn_detached(const std::string& program, const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // The grandchild reports an exec failure through this pipe; a successful exec
    // closes it (O_CLOEXEC) and the read below sees EOF.
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;

    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (child == 0) {
        // Double fork: the host is re-parented to init and never becomes our zombie.
        setsid();
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(grandchild < 0 ? 1 : 0);
        execvp(argv[0], argv.data());
        int err = errno;
        ssize_t ignored = write(fds[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }
    close(fds[1]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    int err = 0;
    ssize_t n;
    do {
        n = read(fds[0], &err, sizeof err);
    } while (n < 0 && errno == EINTR);
    close(fds[0]);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == 0;
}

struct Thread {
    pthread_t handle;
    void (*fn)(void*);
    void* arg;
};

namespace {
void* thread_trampoline(void* p) {
    auto* t = static_cast<Thread*>(p);
    t->fn(t->arg);
    return nullptr;
}
} // namespace

Thread* thread_start(void (*fn)(void*), void* arg) {
    auto* t = new Thread{{}, fn, arg};
    // The agent thread blocks all signals so the app's own signal handling is unaffected.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int rc = pthread_create(&t->handle, nullptr, thread_trampoline, t);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    if (rc != 0) {
        delete t;
        return nullptr;
    }
    pthread_setname_np(t->handle, "hotpp-agent");
    return t;
}

void thread_join(Thread* t) {
    pthread_join(t->handle, nullptr);
    delete t;
}

void sleep_ms(unsigned ms) {
    timespec ts{time_t(ms / 1000), long(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

} // namespace hotpp::agent
