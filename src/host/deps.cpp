#include "host/deps.h"

#include "host/platform.h"

#include <atomic>
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

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

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
                const auto deps = parse_depfile(read_file(depfile), cmd.directory);
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
            graph.set(cmd.file, parse_depfile(read_file(depfile), cmd.directory));
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, jobs); ++t) threads.emplace_back(worker);
    for (std::thread& t : threads) t.join();
    return failed;
}

} // namespace hotpp
