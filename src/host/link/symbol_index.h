// What exists in the running process: the executable's symbols plus everything
// earlier patches added, with every version of each patched function.
#pragma once

#include "host/link/patch_site.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hotpp::image {
class ModuleImage;
}

namespace hotpp::link {

enum class SymbolKind : uint8_t { Function, Data, Tls, Other };

struct FunctionVersion {
    uint64_t entry = 0;
    std::optional<SiteLayout> site; // nullopt: no padding, can't be redirected
};

struct IndexEntry {
    SymbolKind kind = SymbolKind::Other;
    uint64_t address = 0; // canonical address: the one old code and pointers use
    uint64_t size = 0;
    bool writable = false;
    bool ambiguous = false;               // several local symbols share this key
    std::vector<FunctionVersion> versions; // functions: [0] original, back() current

    uint64_t current() const { return versions.empty() ? address : versions.back().entry; }
};

// Identity of a symbol: globals by name, locals by (defining file, name).
struct SymbolKey {
    bool local = false;
    std::string file;
    std::string name;

    std::string str() const { return local ? file + "\x1f" + name : name; }
    std::string display() const { return local ? name + " (" + file + ")" : name; }
};

// Changes a patch makes to the index, applied once the agent confirms the patch.
struct IndexDelta {
    struct NewVersion {
        SymbolKey key;
        FunctionVersion version;
    };
    struct NewSymbol {
        SymbolKey key;
        IndexEntry entry;
    };
    std::vector<NewVersion> versions;
    std::vector<NewSymbol> symbols;
};

class SymbolIndex {
public:
    // Adds a module's symbols; `base` is its load bias (ELF) or image base (PE).
    void add_image(const image::ModuleImage& image, uint64_t base);

    // Adds one symbol; an existing local with the same key becomes ambiguous.
    void add(const SymbolKey& key, const IndexEntry& entry);
    const IndexEntry* find(const SymbolKey& key) const;
    // A global whose name starts with `prefix` (and isn't `except`), if any.
    const IndexEntry* find_global_prefix(const std::string& prefix, const std::string& except, std::string& name) const;
    // Points an existing variable at different storage (where the dynamic linker
    // actually bound it). False if there is no such entry.
    bool rebind(const SymbolKey& key, uint64_t address);
    void apply(const IndexDelta& delta);
    size_t size() const { return entries_.size(); }

private:
    std::unordered_map<std::string, IndexEntry> entries_;
};

} // namespace hotpp::link
