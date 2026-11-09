// A module of the running program (executable or library) as read from disk:
// ELF with DWARF (elf_image.h) or PE with its PDB (pe_image.h). The symbol index,
// layout warnings and program membership use only this.
#pragma once

#include "host/image/dwarf_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace hotpp::image {

// ELF-shaped (elf::STB_*, elf::STT_*): PE images map their PDB symbols onto it.
struct ImageSymbol {
    std::string name;
    std::string file; // the source file that scopes a local symbol
    uint64_t value = 0; // link-time address (ELF) or RVA (PE); TLS: offset in the TLS block
    uint64_t size = 0;
    uint8_t bind = 0, type = 0, visibility = 0;
    uint32_t shndx = 0;
};

// How functions are made redirectable: -fpatchable-function-entry (NOPs before and
// at the entry), or /hotpatch + /FUNCTIONPADMIN (padding before the entry, whose
// first instruction is at least 2 bytes long).
enum class SiteStyle { PatchableEntry, Hotpatch };

class ModuleImage {
public:
    virtual ~ModuleImage() = default;
    virtual const std::vector<ImageSymbol>& symbols() const = 0;
    // Identifies the build: the GNU build ID, or the CodeView {GUID, age}.
    virtual const std::vector<uint8_t>& build_id() const = 0;
    // Absolute source paths of the compile units; empty without debug info.
    virtual const std::vector<std::filesystem::path>& compile_units() const = 0;
    // Why there are no compile units, if there are none.
    virtual const std::string& debug_warning() const = 0;
    virtual bool section_writable(uint32_t shndx) const = 0;
    // Copies `n` bytes at link-time address (ELF) or RVA (PE) `vaddr` from the file.
    virtual bool read(uint64_t vaddr, size_t n, uint8_t* out) const = 0;
    virtual SiteStyle site_style() const = 0;
    // Struct layouts for layout warnings; throws on malformed debug info.
    virtual std::unordered_map<std::string, TypeLayout> type_layouts() const = 0;
};

// ELF or PE, by the file's first bytes. Throws std::runtime_error.
std::unique_ptr<ModuleImage> load_module_image(const std::filesystem::path& path);

} // namespace hotpp::image
