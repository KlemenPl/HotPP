// Which headers each translation unit includes, from the compiler's own
// dependency output (-MM), so a header edit recompiles exactly its includers.
#pragma once

#include "host/compile_db.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace hotpp {

// Paths listed in a make-style dependency file (`target: dep dep \`), relative
// paths resolved against `base`. The target itself is not included.
std::vector<std::filesystem::path> parse_depfile(const std::string& text, const std::filesystem::path& base);

// Arguments that make the compiler write `cmd`'s non-system dependencies to
// `depfile` without compiling.
std::vector<std::string> dependency_scan_arguments(const CompileCommand& cmd, const std::filesystem::path& depfile);

class DependencyGraph {
public:
    // Replaces what `tu` includes. `deps` may contain the TU itself; it's ignored.
    void set(const std::filesystem::path& tu, const std::vector<std::filesystem::path>& deps);
    // Translation units that include `header`, directly or indirectly.
    std::vector<std::filesystem::path> includers(const std::filesystem::path& header) const;
    // Every header any TU includes.
    std::set<std::filesystem::path> headers() const;

private:
    mutable std::mutex mutex_;
    std::map<std::filesystem::path, std::set<std::filesystem::path>> deps_;
    std::map<std::filesystem::path, std::set<std::filesystem::path>> includers_;
};

// Fills `graph` for `units`, running up to `jobs` compilers at once. Results are
// cached under `cache_dir` and reused while no listed file is newer than the cache.
// Returns the number of units whose scan failed (they are left out of the graph).
// `cancel` stops it early when set.
size_t scan_dependencies(const std::vector<const CompileCommand*>& units, const std::filesystem::path& cache_dir,
                         unsigned jobs, DependencyGraph& graph, const std::atomic<bool>* cancel = nullptr);

} // namespace hotpp
