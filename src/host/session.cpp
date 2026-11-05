#include "host/session.h"

#include "host/deps.h"
#include "host/session_state.h"
#include "host/image/dwarf_types.h"
#include "host/link/linker.h"
#include "host/object/elf_object.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

#include <csignal>

#include <unistd.h>

namespace hotpp::host {

std::atomic<int> g_restart_requests{0};

namespace fs = std::filesystem;
using namespace hotpp::proto;
using Clock = std::chrono::steady_clock;

namespace {

std::string format_size(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

const char* platform_name(Platform p) {
    switch (p) {
    case Platform::Linux: return "linux";
    default: return "unknown-os";
    }
}

const char* arch_name(Arch a) { return a == Arch::X64 ? "x64" : "unknown-arch"; }

uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Prints what the agent reported: one line, or everything with --verbose. Returns
// false if it can't be patched: no region, or a region outside rel32 reach.
bool print_hello(const Hello& h, bool verbose) {
    std::string out;
    char line[1024];
    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
    bool ok = !h.regions.empty();
    for (const RegionRecord& r : h.regions) {
        if (r.module_index >= h.modules.size()) {
            ok = false;
            continue;
        }
        const ModuleRecord& m = h.modules[r.module_index];
        ok = ok && std::max(distance(r.base, m.start + m.size), distance(r.base + r.size, m.start)) < (uint64_t(1) << 31);
    }
    if (!verbose && ok) {
        add("[hotpp] agent connected: pid %u, %s (%s patch region)\n", unsigned(h.pid),
            fs::path(h.exe).filename().c_str(), format_size(h.regions[0].size).c_str());
        std::fputs(out.c_str(), stdout);
        std::fflush(stdout);
        return ok;
    }
    add("[hotpp] agent connected: pid %u (%s %s)\n", unsigned(h.pid), platform_name(h.platform),
        arch_name(h.arch));
    add("  exe:              %s\n", h.exe.c_str());
    add("  compile commands: %s\n", h.compile_commands.empty() ? "(not set)" : h.compile_commands.c_str());
    add("  modules (%zu):\n", h.modules.size());
    for (size_t i = 0; i < h.modules.size(); ++i) {
        const ModuleRecord& m = h.modules[i];
        add("    [%2zu] %016llx-%016llx %10s  %s\n", i, (unsigned long long)m.start,
            (unsigned long long)(m.start + m.size), format_size(m.size).c_str(),
            m.path.empty() ? "(unnamed)" : m.path.c_str());
    }
    add("  patch regions (%zu):\n", h.regions.size());
    for (const RegionRecord& r : h.regions) {
        if (r.module_index >= h.modules.size()) {
            add("    %016llx  invalid module index %u\n", (unsigned long long)r.base, unsigned(r.module_index));
            continue;
        }
        const ModuleRecord& m = h.modules[r.module_index];
        const uint64_t farthest = std::max(distance(r.base, m.start + m.size), distance(r.base + r.size, m.start));
        const bool in_reach = farthest < (uint64_t(1) << 31);
        add("    %016llx-%016llx %10s  near [%u], farthest byte %s away%s\n", (unsigned long long)r.base,
            (unsigned long long)(r.base + r.size), format_size(r.size).c_str(), unsigned(r.module_index),
            format_size(farthest).c_str(), in_reach ? "" : "  OUT OF RANGE");
    }
    if (h.regions.empty()) out += "  error: the agent reserved no patch region\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    return ok;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_header(const fs::path& p) {
    static const std::set<std::string> kExt = {".h", ".hh", ".hpp", ".hxx", ".h++", ".inl", ".ipp", ".tpp"};
    return kExt.count(p.extension().string()) != 0;
}

std::string ms_since(Clock::time_point t) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count()) + " ms";
}

std::string plural(size_t n, const char* word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

} // namespace

Session::Session(std::unique_ptr<Stream> stream, HostOptions options)
    : stream_(std::move(stream)), options_(options) {}

Session::~Session() {
    stop_ = true;
    scan_cv_.notify_all();
    if (watch_thread_.joinable()) watch_thread_.join();
    if (scan_thread_.joinable()) scan_thread_.join();
    if (!patch_dir_.empty()) {
        // While the process lives, keep the state file so a new host can resume
        // (docs/DESIGN.md §3.3); the compiled objects can go.
        std::error_code ec;
        const bool alive = hello_.pid && (::kill(pid_t(hello_.pid), 0) == 0 || errno == EPERM);
        if (!alive) {
            fs::remove_all(patch_dir_, ec);
        } else {
            for (const auto& e : fs::directory_iterator(patch_dir_, ec))
                if (e.path().filename().string().rfind("patch-", 0) == 0) fs::remove_all(e.path(), ec);
        }
    }
}

void Session::say(const std::string& line) const {
    // Colors only on a terminal (and not with NO_COLOR): green for applied patches,
    // red for patches not applied, yellow for warnings and notes.
    static const bool color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    const char* c = nullptr;
    if (color) {
        if (line.rfind("warning:", 0) == 0 || line.rfind("note:", 0) == 0) c = "\033[33m";
        else if (line.find(" not applied") != std::string::npos || line.find(" failed") != std::string::npos) c = "\033[31m";
        else if (line.rfind("patch ", 0) == 0 && line.find(" applied") != std::string::npos) c = "\033[32m";
    }
    if (c) std::printf("[hotpp] %s%s\033[0m\n", c, line.c_str());
    else std::printf("[hotpp] %s\n", line.c_str());
    std::fflush(stdout);
}

// Structs whose layout in the patch differs from the running program's: objects
// created before the patch keep the old layout, which the new code would misread.
void Session::warn_layout_changes(ModuleState& module, const link::LinkResult& linked) {
    {
        std::unique_lock<std::mutex> lock(scan_mutex_);
        scan_cv_.wait(lock, [&] { return layouts_ready_ || stop_.load(); });
    }
    if (!module.layouts_ok) return;
    std::set<std::string> reported;
    for (const auto& object : linked.patch.debug_objects) {
        std::unordered_map<std::string, image::TypeLayout> mine;
        try {
            mine = image::dwarf_type_layouts(elf::ElfFile::from_bytes(object, "patch object"));
        } catch (const std::exception&) {
            continue;
        }
        for (auto& [name, layout] : mine) {
            auto old = module.layouts.find(name);
            if (old == module.layouts.end() || old->second == layout || !reported.insert(name).second) continue;
            std::string what = old->second.size != layout.size
                                   ? std::to_string(old->second.size) + " -> " + std::to_string(layout.size) + " bytes"
                                   : "members moved";
            say("warning: layout of " + name + " changed (" + what +
                "); objects created before this patch still have the old layout, restart if any are in use");
            module.pending_layouts[name] = layout;
        }
    }
}

bool Session::send(MsgType type, const Writer& w) {
    uint8_t header[kHeaderSize];
    encode_header(header, type, uint32_t(w.size()));
    std::lock_guard<std::mutex> lock(send_mutex_);
    return w.ok() && stream_->send(header, sizeof header) && (w.size() == 0 || stream_->send(w.data(), w.size()));
}

bool Session::recv(Header& h, std::vector<uint8_t>& payload) {
    uint8_t header[kHeaderSize];
    if (!stream_->recv(header, sizeof header)) return false;
    if (!decode_header(header, h)) {
        std::fprintf(stderr, "[hotpp] bad message header from agent\n");
        return false;
    }
    if (h.version != kVersion) {
        std::fprintf(stderr, "[hotpp] agent speaks protocol version %u, host speaks %u\n", unsigned(h.version),
                     unsigned(kVersion));
        return false;
    }
    payload.resize(h.size);
    return h.size == 0 || stream_->recv(payload.data(), h.size);
}

Session::Outcome Session::run() {
    Outcome outcome;
    Header h;
    std::vector<uint8_t> payload;
    if (!recv(h, payload)) return outcome; // closed without a word, e.g. another host probing
    if (h.type != MsgType::Hello) {
        std::fprintf(stderr, "[hotpp] client sent message type %u before hello; dropped\n", unsigned(h.type));
        return outcome;
    }
    Reader r(payload.data(), payload.size());
    if (!decode(r, hello_)) {
        std::fprintf(stderr, "[hotpp] malformed hello; dropped\n");
        return outcome;
    }
    outcome.hello = true;
    outcome.layout_ok = print_hello(hello_, options_.verbose);
    exe_name_ = fs::path(hello_.exe).filename().string();

    HelloAck ack;
    ack.host_pid = current_pid();
    ack.host_version = HOTPP_VERSION;
    Writer w;
    encode(w, ack);
    if (send(MsgType::HelloAck, w)) {
        if (outcome.layout_ok) {
            try {
                start_hot_reload();
            } catch (const std::exception& e) {
                say("hot reload unavailable for pid " + std::to_string(hello_.pid) + ": " + e.what());
            }
        }
        while (recv(h, payload)) {
            if (h.type == MsgType::Bye) break;
            Reader mr(payload.data(), payload.size());
            if (h.type == MsgType::Log) {
                LogMsg msg;
                if (decode(mr, msg)) std::printf("[agent %u] %s\n", unsigned(hello_.pid), msg.text.c_str());
            } else if (h.type == MsgType::PatchResult) {
                PatchResult res;
                if (decode(mr, res)) {
                    std::lock_guard<std::mutex> lock(reply_mutex_);
                    results_[res.id] = res;
                }
                reply_cv_.notify_all();
            } else if (h.type == MsgType::ModulesChanged) {
                ModulesChanged change;
                if (decode(mr, change)) {
                    std::lock_guard<std::mutex> lock(reply_mutex_);
                    module_changes_.push_back(std::move(change));
                    has_module_changes_ = true;
                }
            } else if (h.type == MsgType::ReserveReply) {
                ReserveReply rep;
                if (decode(mr, rep)) {
                    std::lock_guard<std::mutex> lock(reply_mutex_);
                    reserved_[rep.id] = rep;
                }
                reply_cv_.notify_all();
            } else if (h.type == MsgType::ResolveReply) {
                ResolveReply rep;
                if (decode(mr, rep)) {
                    std::lock_guard<std::mutex> lock(reply_mutex_);
                    resolved_[rep.id] = std::move(rep);
                }
                reply_cv_.notify_all();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        disconnected_ = true;
    }
    reply_cv_.notify_all();
    stop_ = true;
    scan_cv_.notify_all();
    if (watch_thread_.joinable()) watch_thread_.join();
    if (scan_thread_.joinable()) scan_thread_.join();
    say("agent disconnected: pid " + std::to_string(hello_.pid));
    return outcome;
}

void Session::start_hot_reload() {
    if (hello_.platform != Platform::Linux) throw std::runtime_error("only Linux processes can be patched");
    if (hello_.arch != Arch::X64) throw std::runtime_error("only x86-64 is supported");
    if (hello_.compile_commands.empty())
        throw std::runtime_error("the app reported no compile_commands.json (use hotpp_enable() or set HOTPP_COMPILE_COMMANDS)");
    if (hello_.modules.empty() || hello_.regions.empty()) throw std::runtime_error("the app reported no patch region");
    db_ = CompileDb::load(hello_.compile_commands);
    std::error_code time_ec;
    db_time_ = fs::last_write_time(hello_.compile_commands, time_ec);
    patch_dir_ = fs::path(hello_.compile_commands).parent_path() / ".hotpp" / std::to_string(hello_.pid);
    // Module setup asks the agent for patch regions, whose replies the connection
    // thread reads, so it runs on the watcher thread.
    watch_thread_ = std::thread([this] {
        try {
            setup_modules();
        } catch (const std::exception& e) {
            say("hot reload unavailable for pid " + std::to_string(hello_.pid) + ": " + e.what());
            return;
        }
        watch_loop();
    });
}

namespace {
// System libraries are never built from the project's sources; skip reading them.
bool system_library(const std::string& path) {
    for (const char* prefix : {"/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/", "/usr/libexec/"})
        if (path.rfind(prefix, 0) == 0) return true;
    return path.empty() || path.find("linux-vdso") != std::string::npos;
}
} // namespace

// Starts tracking module i if it's built from sources in compile_commands.json.
// Returns it (also added to modules_), or nullptr if it isn't ours or can't be
// patched. Problems with the executable throw.
Session::ModuleState* Session::track_module(size_t i) {
    const ModuleRecord& rec = hello_.modules[i];
    const bool exe = i == 0;
    if (rec.size == 0 || (!exe && system_library(rec.path))) return nullptr;
    auto m = std::make_shared<ModuleState>();
    m->index = i;
    m->name = fs::path(rec.path).filename().string();
    try {
        m->image = image::ElfImage::load(rec.path);
    } catch (const std::exception&) {
        if (exe) throw;
        return nullptr; // stripped or unreadable: not ours
    }
    for (const fs::path& cu : m->image->compile_units()) m->units.insert(normalize_path(cu));
    bool ours = false;
    for (const fs::path& u : m->units) ours |= db_->find(u) != nullptr;
    if (exe && m->units.empty()) {
        say("warning: " + m->name + " has no usable DWARF info" +
            (m->image->dwarf_warning().empty() ? "" : " (" + m->image->dwarf_warning() + ")") +
            "; every file in compile_commands.json is treated as part of it");
        for (const CompileCommand& c : db_->entries()) m->units.insert(c.file);
        ours = true;
    }
    if (!ours) {
        if (exe) throw std::runtime_error("none of the sources " + m->name + " was built from are in " + hello_.compile_commands);
        return nullptr;
    }
    if (!rec.build_id.empty() && !m->image->build_id().empty() && rec.build_id != m->image->build_id()) {
        if (exe) throw std::runtime_error(rec.path + " was rebuilt since the program started; restart it to hot reload");
        say("warning: " + rec.path + " was rebuilt since it was loaded; not patching it");
        return nullptr;
    }
    RegionRecord region;
    const auto existing = std::find_if(hello_.regions.begin(), hello_.regions.end(),
                                       [&](const RegionRecord& r) { return r.module_index == i; });
    if (existing != hello_.regions.end()) {
        region = *existing; // the executable's, or one reserved for an earlier host session
    } else if (exe) {
        throw std::runtime_error("the app reported no patch region for " + m->name);
    } else if (auto r = reserve_region(uint32_t(i), hello_.regions[0].size)) {
        region = *r;
        hello_.regions.push_back(region);
    } else {
        say("warning: no room for a patch region next to " + m->name + "; not patching it");
        return nullptr;
    }
    m->region_base = region.base;
    m->cursor = region.base + region.used;
    m->end = region.base + region.size;
    m->symbols.add_image(*m->image, rec.base);
    if (!exe) bind_interposed_variables(*m);
    for (const fs::path& u : m->units) units_.insert(u);
    modules_.push_back(std::move(m));
    return modules_.back().get();
}

void Session::setup_modules() {
    for (size_t i = 0; i < hello_.modules.size() && !stop_; ++i) track_module(i);

    resume_or_reset();

    std::vector<const CompileCommand*> program;
    for (const CompileCommand& c : db_->entries()) {
        if (!units_.count(c.file)) continue;
        std::error_code ec;
        if (!fs::exists(c.file, ec)) continue;
        watched_dirs_.insert(c.file.parent_path());
        content_hash_[c.file.string()] = std::hash<std::string>()(read_file(c.file));
        program.push_back(&c);
    }
    std::string error;
    watcher_ = watch_directories({watched_dirs_.begin(), watched_dirs_.end()}, error);
    if (!watcher_) throw std::runtime_error(error);

    std::string names;
    size_t symbols = 0;
    for (const auto& m : modules_) {
        names += (names.empty() ? "" : " + ") + m->name;
        symbols += m->symbols.size();
    }
    say("watching " + plural(program.size(), "source file") + " of " + names + " (" + plural(symbols, "symbol") +
        " indexed)");

    // Which headers each source includes (header edits wait for this), then the
    // struct layouts for layout-change warnings.
    const fs::path cache = fs::path(hello_.compile_commands).parent_path() / ".hotpp" / "deps";
    scan_thread_ = std::thread([this, program, cache, modules = modules_] {
        const auto started = Clock::now();
        const size_t failed = scan_dependencies(program, cache, std::max(1u, std::thread::hardware_concurrency()),
                                                graph_, &stop_);
        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            scan_done_ = true;
        }
        scan_cv_.notify_all();
        if (!stop_)
            say("tracking " + plural(graph_.headers().size(), "header") + " (dependency scan took " + ms_since(started) +
                (failed ? ", " + plural(failed, "source") + " failed to scan" : std::string()) + ")");
        for (const auto& m : modules) { // the snapshot: modules_ may change meanwhile
            if (stop_) break;
            try {
                m->layouts = image::dwarf_type_layouts(m->image->file());
                m->layouts_ok = true;
            } catch (const std::exception& e) {
                say("warning: can't read struct layouts of " + m->name + " (" + e.what() + "); no layout warnings");
            }
        }
        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            layouts_ready_ = true;
        }
        scan_cv_.notify_all();
    });
}

// A shared library's exported variables may live elsewhere: a PIE executable that
// uses one gets its own copy (a copy relocation), and the dynamic linker binds the
// library's references to that copy. The agent's dlsym says where each one is.
void Session::bind_interposed_variables(ModuleState& m) {
    std::vector<std::string> names;
    for (const image::ImageSymbol& sym : m.image->symbols())
        if (sym.bind != elf::STB_LOCAL && sym.visibility == elf::STV_DEFAULT && sym.type == elf::STT_OBJECT)
            names.push_back(sym.name);
    if (names.empty()) return;
    std::vector<uint64_t> addrs;
    try {
        addrs = resolve_external(names);
    } catch (const std::exception& e) {
        say("warning: can't look up where " + m.name + "'s variables live (" + e.what() + ")");
        return;
    }
    size_t moved = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const link::SymbolKey key{false, "", names[i]};
        const link::IndexEntry* e = m.symbols.find(key);
        if (addrs[i] && e && e->address != addrs[i] && m.symbols.rebind(key, addrs[i])) ++moved;
    }
    if (moved && options_.verbose)
        say(plural(moved, "variable") + " of " + m.name + " live in another module (copy relocations)");
}

// A reconnecting agent (the host was restarted) may already carry patches. Resume
// from the state the previous session saved, if it matches what the agent reports;
// otherwise refuse, since new code could land on live code and the old patches'
// symbols would be unknown.
void Session::resume_or_reset() {
    const fs::path file = patch_dir_ / "state";
    std::error_code ec;
    bool patched = false;
    for (const RegionRecord& r : hello_.regions) patched |= r.used > 0;
    if (!hello_.resumed) {
        fs::remove(file, ec); // a new process (or a hot restart) with this pid
        return;
    }
    const std::optional<SessionState> st = load_session_state(file);
    if (!st) {
        if (!patched) return; // reconnected before anything was patched
        throw std::runtime_error("the program was patched by an earlier host session whose state is gone; "
                                 "restart it to keep hot reloading");
    }
    auto mismatch = [] {
        return std::runtime_error("the state saved by the earlier host session doesn't match the program; "
                                  "restart it to keep hot reloading");
    };
    if (st->pid != hello_.pid || st->modules.size() > modules_.size()) throw mismatch();
    for (const SavedModule& saved : st->modules) {
        auto m = std::find_if(modules_.begin(), modules_.end(), [&](const auto& x) { return x->index == saved.index; });
        if (m == modules_.end() || saved.index >= hello_.modules.size()) throw mismatch();
        const ModuleRecord& rec = hello_.modules[saved.index];
        if (saved.path != rec.path || saved.build_id != rec.build_id || saved.region_base != (*m)->region_base ||
            saved.cursor != (*m)->cursor)
            throw mismatch();
    }
    for (const auto& [index, delta] : st->deltas) {
        auto m = std::find_if(modules_.begin(), modules_.end(), [&](const auto& x) { return x->index == index; });
        if (m == modules_.end()) throw mismatch();
        (*m)->symbols.apply(delta);
    }
    applied_ = st->deltas;
    next_patch_ = st->next_patch;
    say("resumed the earlier host session's patches; the next is patch " + std::to_string(next_patch_));
}

void Session::save_state() {
    SessionState st;
    st.pid = hello_.pid;
    st.next_patch = next_patch_;
    for (const auto& m : modules_) {
        SavedModule s;
        s.index = uint32_t(m->index);
        s.path = hello_.modules[m->index].path;
        s.build_id = hello_.modules[m->index].build_id;
        s.region_base = m->region_base;
        s.cursor = m->cursor;
        st.modules.push_back(std::move(s));
    }
    st.deltas = applied_;
    if (!save_session_state(patch_dir_ / "state", st)) say("warning: can't save the session state in " + patch_dir_.string());
}

std::optional<RegionRecord> Session::reserve_region(uint32_t module_index, uint64_t size) {
    ReserveRequest req;
    req.id = next_request_++;
    req.module_index = module_index;
    req.size = size;
    Writer w;
    encode(w, req);
    if (!send(MsgType::ReserveRequest, w)) return std::nullopt;
    std::unique_lock<std::mutex> lock(reply_mutex_);
    reply_cv_.wait_for(lock, std::chrono::seconds(10), [&] { return disconnected_ || reserved_.count(req.id); });
    auto it = reserved_.find(req.id);
    if (it == reserved_.end() || !it->second.ok) return std::nullopt;
    RegionRecord r = it->second.region;
    reserved_.erase(it);
    return r;
}

void Session::watch_headers() {
    for (const fs::path& h : graph_.headers()) {
        content_hash_.emplace(h.string(), std::hash<std::string>()(read_file(h)));
        const fs::path dir = h.parent_path();
        if (!watched_dirs_.insert(dir).second) continue;
        std::string error;
        if (!watcher_->add(dir, error)) say("warning: " + error);
    }
}

// Libraries the program loaded or unloaded at runtime (dlopen/dlclose).
void Session::apply_module_changes() {
    std::vector<ModulesChanged> changes;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        changes.swap(module_changes_);
    }
    for (const ModulesChanged& c : changes) {
        for (uint32_t i : c.removed) {
            if (i >= hello_.modules.size()) continue;
            hello_.modules[i].size = 0;
            auto it = std::find_if(modules_.begin(), modules_.end(), [&](const auto& m) { return m->index == i; });
            if (it == modules_.end()) continue;
            say("stopped patching " + (*it)->name + " (unloaded)");
            modules_.erase(it);
        }
        units_.clear();
        for (const auto& m : modules_) units_.insert(m->units.begin(), m->units.end());

        hello_.modules.resize(c.first_index); // the agent's indices; extra slots would be placeholders
        for (const ModuleRecord& rec : c.added) hello_.modules.push_back(rec);
        for (size_t i = c.first_index; i < hello_.modules.size(); ++i) {
            ModuleState* m = nullptr;
            try {
                m = track_module(i);
            } catch (const std::exception& e) {
                say("warning: can't patch " + hello_.modules[i].path + ": " + e.what());
            }
            if (!m) continue;
            std::vector<const CompileCommand*> sources;
            for (const fs::path& u : m->units)
                if (const CompileCommand* cmd = db_->find(u)) {
                    sources.push_back(cmd);
                    content_hash_.emplace(cmd->file.string(), std::hash<std::string>()(read_file(cmd->file)));
                    if (watched_dirs_.insert(cmd->file.parent_path()).second) {
                        std::string error;
                        if (!watcher_->add(cmd->file.parent_path(), error)) say("warning: " + error);
                    }
                }
            scan_dependencies(sources, fs::path(hello_.compile_commands).parent_path() / ".hotpp" / "deps",
                              std::max(1u, std::thread::hardware_concurrency()), graph_, &stop_);
            watch_headers();
            try {
                m->layouts = image::dwarf_type_layouts(m->image->file());
                m->layouts_ok = true;
            } catch (const std::exception&) {
            }
            say("watching " + plural(sources.size(), "source file") + " of " + m->name + " (loaded at runtime)");
        }
    }
}

void Session::watch_loop() {
    using namespace std::chrono_literals;
    bool headers_watched = false;
    int restarts_seen = g_restart_requests.load();
    while (!stop_) {
        if (has_module_changes_.exchange(false)) apply_module_changes();
        if (g_restart_requests.load() != restarts_seen) {
            restarts_seen = g_restart_requests.load();
            hot_restart("requested");
            continue;
        }
        if (!headers_watched && scan_done_) {
            watch_headers();
            headers_watched = true;
        }
        const std::vector<fs::path> files = watcher_->wait(200ms, 100ms);
        if (!files.empty() && !stop_) handle_changes(files);
    }
}

bool Session::changed(const fs::path& file) {
    const size_t h = std::hash<std::string>()(read_file(file));
    auto [it, inserted] = content_hash_.emplace(file.string(), h);
    if (inserted) return true;
    if (it->second == h) return false; // saved without changes
    it->second = h;
    return true;
}

// A re-run of the build system rewrites compile_commands.json (new flags, new
// files); later patches use the new commands.
void Session::reload_compile_db_if_changed() {
    std::error_code ec;
    const auto t = fs::last_write_time(hello_.compile_commands, ec);
    if (ec || t == db_time_) return;
    try {
        db_ = CompileDb::load(hello_.compile_commands);
        db_time_ = t;
        if (options_.verbose) say("reloaded " + hello_.compile_commands);
    } catch (const std::exception& e) {
        say(std::string("warning: keeping the previous compile commands: ") + e.what());
    }
}

void Session::handle_changes(const std::vector<fs::path>& files) {
    const auto started = Clock::now();
    reload_compile_db_if_changed();
    std::vector<const CompileCommand*> tus;
    std::set<fs::path> picked;
    auto add_tu = [&](const fs::path& file) {
        const CompileCommand* cmd = db_->find(file);
        if (!cmd || !units_.count(cmd->file)) return; // another program's source
        if (picked.insert(cmd->file).second) tus.push_back(cmd);
    };

    for (const fs::path& f : files) {
        if (db_->find(f)) {
            if (changed(f)) add_tu(f);
            continue;
        }
        if (!is_header(f) && !graph_.headers().count(f)) continue; // not something the program is built from
        if (!scan_done_) {
            say("waiting for the dependency scan to finish before applying the change to " + f.filename().string());
            std::unique_lock<std::mutex> lock(scan_mutex_);
            scan_cv_.wait(lock, [&] { return scan_done_ || stop_.load(); });
            if (stop_) return;
        }
        const std::vector<fs::path> includers = graph_.includers(f);
        if (includers.empty()) continue; // not included by this program
        if (!changed(f)) continue;
        size_t before = tus.size();
        for (const fs::path& tu : includers) add_tu(tu);
        say(f.filename().string() + " changed: recompiling " + plural(tus.size() - before, "file") + " that include it");
    }
    if (tus.empty()) return;

    const uint32_t id = next_patch_++;
    const fs::path dir = patch_dir_ / ("patch-" + std::to_string(id));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        say("patch " + std::to_string(id) + " not applied: cannot create " + dir.string() + ": " + ec.message());
        return;
    }

    // Compile all affected sources in parallel.
    struct Job {
        const CompileCommand* cmd;
        fs::path out, depfile;
        std::string output, error;
        int rc = -1;
    };
    std::vector<Job> jobs(tus.size());
    std::string names;
    for (size_t i = 0; i < tus.size(); ++i) {
        jobs[i].cmd = tus[i];
        const std::string stem = std::to_string(i) + "-" + tus[i]->file.stem().string();
        jobs[i].out = dir / (stem + ".o");
        jobs[i].depfile = dir / (stem + ".d");
        names += (names.empty() ? "" : ", ") + tus[i]->file.filename().string();
    }
    say("compiling " + names);
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        const unsigned n = std::min<unsigned>(unsigned(jobs.size()), std::max(1u, std::thread::hardware_concurrency()));
        for (unsigned t = 0; t < n; ++t)
            workers.emplace_back([&] {
                for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                    Job& j = jobs[i];
                    std::vector<std::string> args = patch_compile_arguments(*j.cmd, j.out);
                    args.insert(args.end(), {"-MMD", "-MF", j.depfile.string()});
                    j.rc = run_capture(args, j.cmd->directory, j.output, j.error);
                }
            });
        for (std::thread& w : workers) w.join();
    }
    std::vector<std::unique_ptr<object::ElfObject>> objects;
    bool failed = false;
    for (Job& j : jobs) {
        if (!j.output.empty()) std::fputs(j.output.c_str(), stdout);
        if (j.rc != 0) {
            say("patch " + std::to_string(id) + " not applied: " +
                (j.rc < 0 ? j.error : j.cmd->file.filename().string() + " failed to compile"));
            failed = true;
            continue;
        }
        // Keep the include graph current: the edit may have added or removed includes.
        graph_.set(j.cmd->file, parse_depfile(read_file(j.depfile), j.cmd->directory));
        if (failed) continue;
        try {
            objects.push_back(object::ElfObject::load(j.out));
        } catch (const std::exception& e) {
            say("patch " + std::to_string(id) + " not applied: " + e.what());
            failed = true;
        }
    }
    if (failed) return;
    if (scan_done_) watch_headers(); // newly included headers

    // Link each object against the module it belongs to (a source can be part of
    // several), then send everything as one patch.
    struct ModuleLink {
        ModuleState* module;
        std::vector<const object::ElfObject*> objects;
        link::LinkResult result;
    };
    std::vector<ModuleLink> links;
    for (size_t i = 0; i < jobs.size(); ++i)
        for (const auto& m : modules_) {
            if (!m->units.count(jobs[i].cmd->file)) continue;
            auto it = std::find_if(links.begin(), links.end(), [&](const ModuleLink& l) { return l.module == m.get(); });
            if (it == links.end()) it = links.insert(links.end(), ModuleLink{m.get(), {}, {}});
            it->objects.push_back(objects[i].get());
        }
    try {
        for (ModuleLink& l : links) {
            const ModuleRecord& rec = hello_.modules[l.module->index];
            link::LinkOptions opt;
            opt.region_cursor = l.module->cursor;
            opt.region_end = l.module->end;
            opt.tls_static = rec.tls_static;
            opt.tls_offset = rec.tls_offset;
            opt.tls_module_id = rec.tls_module_id;
            l.result = link::link_patch(
                l.objects, l.module->symbols, [this](const std::vector<std::string>& n) { return resolve_external(n); },
                opt);
        }
    } catch (const link::RestartRequired& e) {
        say("patch " + std::to_string(id) + " not applied, restart required: " + e.what());
        if (options_.auto_restart) hot_restart(e.what());
        else if (!options_.build_command.empty())
            say("run with --auto-restart, or send the host SIGUSR1, to rebuild and restart the program");
        return;
    } catch (const std::exception& e) {
        say("patch " + std::to_string(id) + " not applied: " + e.what());
        return;
    }

    Patch patch;
    patch.id = id;
    size_t replaced = 0, fresh = 0, kept = 0, inits = 0;
    uint64_t bytes = 0;
    for (ModuleLink& l : links) {
        const std::string where = links.size() > 1 ? " [" + l.module->name + "]" : "";
        if (options_.verbose)
            for (const std::string& line : l.result.log) say("  " + line + where);
        for (const std::string& note : l.result.notes) say("note: " + note);
        l.module->pending_layouts.clear();
        warn_layout_changes(*l.module, l.result);
        proto::Patch& p = l.result.patch;
        patch.segments.insert(patch.segments.end(), p.segments.begin(), p.segments.end());
        patch.sites.insert(patch.sites.end(), p.sites.begin(), p.sites.end());
        patch.eh_frames.insert(patch.eh_frames.end(), p.eh_frames.begin(), p.eh_frames.end());
        patch.initializers.insert(patch.initializers.end(), p.initializers.begin(), p.initializers.end());
        for (auto& d : p.debug_objects) patch.debug_objects.push_back(std::move(d));
        replaced += l.result.functions_redirected;
        fresh += l.result.functions_new;
        kept += l.result.data_kept;
        inits += l.result.initializers_run;
        bytes += l.result.bytes_used;
    }

    Writer w;
    encode(w, patch);
    if (!send(MsgType::Patch, w)) return;
    for (ModuleLink& l : links) l.module->cursor += l.result.bytes_used; // reserved even if applying fails
    save_state();

    const std::optional<PatchResult> result = wait_for_result(id);
    if (!result) return; // disconnected
    if (!result->ok) {
        say("patch " + std::to_string(id) + " failed in the app: " + result->error);
        return;
    }
    for (ModuleLink& l : links) {
        l.module->symbols.apply(l.result.delta);
        applied_.emplace_back(uint32_t(l.module->index), l.result.delta);
        for (auto& [name, layout] : l.module->pending_layouts) l.module->layouts[name] = layout; // warn once
    }
    std::string summary = plural(replaced, "function") + " replaced, " + std::to_string(fresh) + " new, " +
                          plural(kept, "variable") + " kept";
    if (inits) summary += ", " + plural(inits, "initializer") + " run";
    if (links.size() > 1) summary += ", " + std::to_string(links.size()) + " modules";
    save_state();
    say("patch " + std::to_string(id) + " applied in " + ms_since(started) + ": " + names + " (" + summary + ", " +
        format_size(bytes) + ")");
}

// Rebuild (if a build command is set), then have the agent re-exec the program. The
// process keeps its pid and reconnects as a new session; this one ends.
void Session::hot_restart(const std::string& reason) {
    if (!options_.build_command.empty()) {
        say("rebuilding (" + reason + "): " + options_.build_command);
        const auto started = Clock::now();
        std::string output, error;
        const int rc = run_capture({"/bin/sh", "-c", options_.build_command}, {}, output, error);
        if (rc != 0) {
            std::fputs(output.c_str(), stdout);
            say("restart cancelled: " + (rc < 0 ? error : "the build failed (exit code " + std::to_string(rc) + ")"));
            return;
        }
        say("rebuilt in " + ms_since(started));
    }
    Writer w;
    if (send(MsgType::Restart, w)) say("restarting " + exe_name_);
}

std::vector<uint64_t> Session::resolve_external(const std::vector<std::string>& names) {
    ResolveRequest req;
    req.id = next_request_++;
    req.names = names;
    Writer w;
    encode(w, req);
    if (!send(MsgType::ResolveRequest, w)) throw link::LinkError("lost the connection to the app");
    std::unique_lock<std::mutex> lock(reply_mutex_);
    const bool got = reply_cv_.wait_for(lock, std::chrono::seconds(10),
                                        [&] { return disconnected_ || resolved_.count(req.id); });
    if (!got || disconnected_) throw link::LinkError("the app did not answer a symbol lookup");
    std::vector<uint64_t> out = std::move(resolved_[req.id].addresses);
    resolved_.erase(req.id);
    if (out.size() != names.size()) throw link::LinkError("malformed symbol lookup reply");
    return out;
}

std::optional<PatchResult> Session::wait_for_result(uint32_t id) {
    std::unique_lock<std::mutex> lock(reply_mutex_);
    auto ready = [&] { return disconnected_ || results_.count(id); };
    if (!reply_cv_.wait_for(lock, std::chrono::seconds(2), ready)) {
        say("patch " + std::to_string(id) + " is waiting for the app to call hotpp::sync_point()");
        reply_cv_.wait(lock, ready);
    }
    if (!results_.count(id)) return std::nullopt;
    PatchResult r = results_[id];
    results_.erase(id);
    return r;
}

} // namespace hotpp::host
