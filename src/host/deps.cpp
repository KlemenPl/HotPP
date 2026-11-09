#include "host/deps.h"

#include "host/platform.h"
#include "host/util/json.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

namespace hotpp {

namespace fs = std::filesystem;

std::vector<fs::path> parse_depfile(const std::string& text, const fs::path& base) {
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) tokens.push_back(std::move(cur));
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == '\n' || (n == '\r' && i + 2 < text.size() && text[i + 2] == '\n')) { // line continuation
                flush();
                i += n == '\r' ? 2 : 1;
                continue;
            }
            if (n == ' ' || n == '#' || n == '\\') {
                cur += n;
                ++i;
                continue;
            }
            cur += c;
        } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') {
            cur += '$';
            ++i;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();

    std::vector<fs::path> out;
    bool after_target = false;
    for (const std::string& t : tokens) {
        if (!after_target) {
            // "target:" or "target" followed by ":"
            if (t == ":") after_target = true;
            else if (t.back() == ':') after_target = true;
            continue;
        }
        if (t.back() == ':') break; // a second rule (e.g. -MP phony targets)
        out.push_back(normalize_path(t, base));
    }
    return out;
}

std::vector<std::string> dependency_scan_arguments(const CompileCommand& cmd, const fs::path& depfile) {
    const Driver driver = driver_of(cmd);
    if (msvc_style(driver)) {
        // Like a patch compile, but only preprocessing / syntax checking.
        std::vector<std::string> out;
        for (const std::string& a : patch_compile_arguments(cmd, fs::path(depfile).replace_extension(".obj"), depfile)) {
            if (a == "/c" || a == "-c" || a == "/Z7" || a == "/Gy" || a == "/Gw" || a.rfind("/Fo", 0) == 0) continue;
            out.push_back(a);
        }
        // cl: /Zs checks syntax without writing an object. clang-cl: -MM preprocesses only.
        auto at = std::find(out.begin(), out.end(), std::string("--"));
        if (driver == Driver::Cl) {
            out.insert(at, "/Zs");
        } else {
            for (std::string& a : out)
                if (a == "/clang:-MMD") a = "/clang:-MM";
        }
        return out;
    }
    std::vector<std::string> out;
    const auto& a = cmd.arguments;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string& s = a[i];
        if (s == "-o" || s == "-MF" || s == "-MT" || s == "-MQ") {
            ++i;
            continue;
        }
        if (s == "-c" || s == "-MD" || s == "-MMD" || s == "-MP" || s.rfind("-MF", 0) == 0 || s.rfind("-MT", 0) == 0 ||
            s.rfind("-MQ", 0) == 0 || (s.size() > 2 && s.rfind("-o", 0) == 0))
            continue;
        out.push_back(s);
    }
    // -MM: dependencies only, leaving out system headers; -MF: where to write them.
    out.push_back("-MM");
    out.push_back("-MF");
    out.push_back(depfile.string());
    return out;
}

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Where cl finds system headers: %INCLUDE%, /external:I and /imsvc directories.
std::vector<fs::path> msvc_system_dirs(const CompileCommand& cmd) {
    std::vector<fs::path> dirs;
    if (const char* inc = std::getenv("INCLUDE")) {
        std::string list = inc;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t end = std::min(list.find(';', start), list.size());
            if (end > start) dirs.push_back(normalize_path(list.substr(start, end - start)));
            start = end + 1;
        }
    }
    const auto& a = cmd.arguments;
    for (size_t i = 1; i < a.size(); ++i) {
        for (const char* flag : {"/external:I", "-external:I", "/imsvc", "-imsvc"}) {
            const size_t n = std::strlen(flag);
            if (a[i].compare(0, n, flag) != 0) continue;
            const std::string dir = a[i].size() > n ? a[i].substr(n) : (i + 1 < a.size() ? a[++i] : std::string());
            if (!dir.empty()) dirs.push_back(normalize_path(dir, cmd.directory));
            break;
        }
    }
    return dirs;
}

bool under(const fs::path& p, const fs::path& dir) {
    auto d = dir.begin(), q = p.begin();
    for (; d != dir.end(); ++d, ++q) {
        if (d->empty()) continue;
        if (q == p.end() || *q != *d) return false;
    }
    return true;
}

} // namespace

std::vector<fs::path> parse_source_dependencies(const std::string& text, const std::vector<fs::path>& system_dirs) {
    std::vector<fs::path> out;
    json::Value doc;
    try {
        doc = json::Value::parse(text);
    } catch (const std::exception&) {
        return out;
    }
    const json::Value* data = doc.get("Data");
    const json::Value* includes = data ? data->get("Includes") : nullptr;
    if (!includes || !includes->is_array()) return out;
    for (const json::Value& v : includes->as_array()) {
        if (!v.is_string()) continue;
        const fs::path p = normalize_path(v.as_string());
        if (std::none_of(system_dirs.begin(), system_dirs.end(), [&](const fs::path& d) { return under(p, d); }))
            out.push_back(p);
    }
    if (const json::Value* source = data->get("Source"); source && source->is_string())
        out.push_back(normalize_path(source->as_string()));
    return out;
}

std::vector<fs::path> read_dependencies(const CompileCommand& cmd, const fs::path& depfile) {
    const std::string text = read_text(depfile);
    if (driver_of(cmd) == Driver::Cl) return parse_source_dependencies(text, msvc_system_dirs(cmd));
    return parse_depfile(text, cmd.directory);
}

void DependencyGraph::set(const fs::path& tu, const std::vector<fs::path>& deps) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const fs::path& old : deps_[tu]) includers_[old].erase(tu);
    std::set<fs::path>& mine = deps_[tu];
    mine.clear();
    for (const fs::path& d : deps) {
        if (d == tu) continue;
        mine.insert(d);
        includers_[d].insert(tu);
    }
}

std::vector<fs::path> DependencyGraph::includers(const fs::path& header) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = includers_.find(header);
    if (it == includers_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

std::set<fs::path> DependencyGraph::headers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<fs::path> out;
    for (const auto& [h, tus] : includers_)
        if (!tus.empty()) out.insert(h);
    return out;
}

namespace {

// A cached depfile is usable if no file it lists (and not the TU) changed after it.
bool cache_fresh(const fs::path& depfile, const fs::path& tu, const std::vector<fs::path>& deps) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(depfile, ec);
    if (ec) return false;
    auto newer = [&](const fs::path& p) {
        std::error_code e;
        const auto t = fs::last_write_time(p, e);
        return e || t > stamp;
    };
    if (newer(tu)) return false;
    for (const fs::path& d : deps)
        if (newer(d)) return false;
    return true;
}

} // namespace

size_t scan_dependencies(const std::vector<const CompileCommand*>& units, const fs::path& cache_dir, unsigned jobs,
                         DependencyGraph& graph, const std::atomic<bool>* cancel) {
    std::error_code dir_ec;
    fs::create_directories(cache_dir, dir_ec);
    std::atomic<size_t> next{0}, failed{0};
    auto worker = [&] {
        std::error_code ec; // per worker: they run concurrently
        for (size_t i; (i = next.fetch_add(1)) < units.size();) {
            if (cancel && *cancel) return;
            const CompileCommand& cmd = *units[i];
            std::string key = cmd.file.string();
            for (const std::string& a : cmd.arguments) key += '\0' + a;
            char name[32];
            std::snprintf(name, sizeof name, "%016zx.d", std::hash<std::string>()(key));
            const fs::path depfile = cache_dir / name;

            if (fs::exists(depfile, ec)) {
                const auto deps = read_dependencies(cmd, depfile);
                if (!deps.empty() && cache_fresh(depfile, cmd.file, deps)) {
                    graph.set(cmd.file, deps);
                    continue;
                }
            }
            std::string output, error;
            const int rc = host::run_capture(dependency_scan_arguments(cmd, depfile), cmd.directory, output, error);
            if (rc != 0) {
                ++failed;
                continue;
            }
            graph.set(cmd.file, read_dependencies(cmd, depfile));
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, jobs); ++t) threads.emplace_back(worker);
    for (std::thread& t : threads) t.join();
    return failed;
}

} // namespace hotpp
