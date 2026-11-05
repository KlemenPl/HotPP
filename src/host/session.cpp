#include "host/session.h"

#include "host/link/linker.h"
#include "host/object/elf_object.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

namespace hotpp::host {

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
    case Platform::Windows: return "windows";
    default: return "unknown-os";
    }
}

const char* arch_name(Arch a) { return a == Arch::X64 ? "x64" : "unknown-arch"; }

uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Prints what the agent reported. Returns false if it can't be patched: no region,
// or a region outside rel32 reach of its module.
bool print_hello(const Hello& h) {
    std::string out;
    char line[1024];
    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
    };
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
    bool ok = !h.regions.empty();
    add("  patch regions (%zu):\n", h.regions.size());
    for (const RegionRecord& r : h.regions) {
        if (r.module_index >= h.modules.size()) {
            add("    %016llx  invalid module index %u\n", (unsigned long long)r.base, unsigned(r.module_index));
            ok = false;
            continue;
        }
        const ModuleRecord& m = h.modules[r.module_index];
        const uint64_t farthest = std::max(distance(r.base, m.start + m.size), distance(r.base + r.size, m.start));
        const bool in_reach = farthest < (uint64_t(1) << 31);
        ok = ok && in_reach;
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
    if (watch_thread_.joinable()) watch_thread_.join();
    if (!patch_dir_.empty()) {
        std::error_code ec;
        fs::remove_all(patch_dir_, ec);
    }
}

void Session::say(const std::string& line) const {
    std::printf("[hotpp] %s\n", line.c_str());
    std::fflush(stdout);
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
    outcome.layout_ok = print_hello(hello_);
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
    if (watch_thread_.joinable()) watch_thread_.join();
    say("agent disconnected: pid " + std::to_string(hello_.pid));
    return outcome;
}

void Session::start_hot_reload() {
    if (hello_.platform != Platform::Linux) throw std::runtime_error("patching Windows processes arrives in M2");
    if (hello_.arch != Arch::X64) throw std::runtime_error("only x86-64 is supported");
    if (hello_.compile_commands.empty())
        throw std::runtime_error("the app reported no compile_commands.json (use hotpp_enable() or set HOTPP_COMPILE_COMMANDS)");
    if (hello_.modules.empty() || hello_.regions.empty()) throw std::runtime_error("the app reported no patch region");

    db_ = CompileDb::load(hello_.compile_commands);
    const ModuleRecord& exe = hello_.modules[0];
    image_ = image::ElfImage::load(exe.path);
    if (!exe.build_id.empty() && !image_->build_id().empty() && exe.build_id != image_->build_id())
        throw std::runtime_error(exe.path + " was rebuilt since the program started; restart it to hot reload");
    index_.add_image(*image_, exe.base);
    region_cursor_ = hello_.regions[0].base;
    region_end_ = hello_.regions[0].base + hello_.regions[0].size;

    for (const fs::path& cu : image_->compile_units()) units_.insert(normalize_path(cu));
    if (units_.empty())
        say("warning: " + exe_name_ + " has no usable DWARF info" +
            (image_->dwarf_warning().empty() ? "" : " (" + image_->dwarf_warning() + ")") +
            "; every file in compile_commands.json is treated as part of it");

    std::set<fs::path> dirs;
    size_t files = 0;
    for (const CompileCommand& c : db_->entries()) {
        if (!units_.empty() && !units_.count(c.file)) continue;
        std::error_code ec;
        if (!fs::exists(c.file, ec)) continue;
        dirs.insert(c.file.parent_path());
        content_hash_[c.file.string()] = std::hash<std::string>()(read_file(c.file));
        ++files;
    }
    if (files == 0)
        throw std::runtime_error("none of the sources " + exe_name_ + " was built from are in " + hello_.compile_commands);

    std::string error;
    watcher_ = watch_directories({dirs.begin(), dirs.end()}, error);
    if (!watcher_) throw std::runtime_error(error);

    patch_dir_ = fs::path(hello_.compile_commands).parent_path() / ".hotpp" / std::to_string(hello_.pid);
    say("watching " + plural(files, "source file") + " of " + exe_name_ + " (" + plural(index_.size(), "symbol") +
        " indexed)");
    watch_thread_ = std::thread([this] { watch_loop(); });
}

void Session::watch_loop() {
    using namespace std::chrono_literals;
    while (!stop_) {
        const std::vector<fs::path> files = watcher_->wait(200ms, 100ms);
        if (!files.empty() && !stop_) handle_changes(files);
    }
}

void Session::handle_changes(const std::vector<fs::path>& files) {
    const auto started = Clock::now();
    std::vector<const CompileCommand*> tus;
    for (const fs::path& f : files) {
        const CompileCommand* cmd = db_->find(f);
        if (!cmd) {
            if (is_header(f)) say("ignoring change to " + f.filename().string() + ": header changes are supported from M3");
            continue;
        }
        if (!units_.empty() && !units_.count(cmd->file)) continue; // another program's source
        const size_t h = std::hash<std::string>()(read_file(cmd->file));
        size_t& known = content_hash_[cmd->file.string()];
        if (known == h) continue; // saved without changes
        known = h;
        tus.push_back(cmd);
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

    std::string names;
    std::vector<std::unique_ptr<object::ElfObject>> objects;
    for (size_t i = 0; i < tus.size(); ++i) {
        const CompileCommand& cmd = *tus[i];
        const std::string name = cmd.file.filename().string();
        names += (names.empty() ? "" : ", ") + name;
        const fs::path out = dir / (std::to_string(i) + "-" + cmd.file.stem().string() + ".o");
        say("compiling " + name);
        std::string output, error;
        const int rc = run_capture(patch_compile_arguments(cmd, out), cmd.directory, output, error);
        if (!output.empty()) std::fputs(output.c_str(), stdout);
        if (rc != 0) {
            say("patch " + std::to_string(id) + " not applied: " + (rc < 0 ? error : name + " failed to compile"));
            return;
        }
        try {
            objects.push_back(object::ElfObject::load(out));
        } catch (const std::exception& e) {
            say("patch " + std::to_string(id) + " not applied: " + e.what());
            return;
        }
    }

    link::LinkResult linked;
    try {
        std::vector<const object::ElfObject*> ptrs;
        for (const auto& o : objects) ptrs.push_back(o.get());
        link::LinkOptions opt;
        opt.region_cursor = region_cursor_;
        opt.region_end = region_end_;
        linked = link::link_patch(
            ptrs, index_, [this](const std::vector<std::string>& n) { return resolve_external(n); }, opt);
    } catch (const link::RestartRequired& e) {
        say("patch " + std::to_string(id) + " not applied, restart required: " + e.what());
        return;
    } catch (const std::exception& e) {
        say("patch " + std::to_string(id) + " not applied: " + e.what());
        return;
    }
    if (options_.verbose)
        for (const std::string& line : linked.log) say("  " + line);
    for (const std::string& note : linked.notes) say("note: " + note);

    linked.patch.id = id;
    Writer w;
    encode(w, linked.patch);
    if (!send(MsgType::Patch, w)) return;
    region_cursor_ += linked.bytes_used; // reserved even if applying fails half way

    const std::optional<PatchResult> result = wait_for_result(id);
    if (!result) return; // disconnected
    if (!result->ok) {
        say("patch " + std::to_string(id) + " failed in the app: " + result->error);
        return;
    }
    index_.apply(linked.delta);
    say("patch " + std::to_string(id) + " applied in " + ms_since(started) + ": " + names + " (" +
        plural(linked.functions_redirected, "function") + " replaced, " + std::to_string(linked.functions_new) +
        " new, " + plural(linked.data_kept, "variable") + " kept, " + format_size(linked.bytes_used) + ")");
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
