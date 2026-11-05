// Struct/class layouts from DWARF, for warning about layout changes (docs/DESIGN.md §7.6).
#pragma once

#include "host/elf/elf_file.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hotpp::image {

struct TypeLayout {
    uint64_t size = 0;
    std::vector<std::pair<std::string, int64_t>> members; // name (or "<base N>") and byte offset

    bool operator==(const TypeLayout& o) const { return size == o.size && members == o.members; }
    bool operator!=(const TypeLayout& o) const { return !(*this == o); }
};

// Every named struct, class and union defined at namespace or class scope, by
// qualified name ("ns::Outer::Inner"). Local and unnamed types are left out; the
// first definition of a name wins. Throws elf::ElfError on malformed DWARF.
std::unordered_map<std::string, TypeLayout> dwarf_type_layouts(const elf::ElfFile& file);

} // namespace hotpp::image
