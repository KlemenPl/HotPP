// The running program's executable, read from disk: its full symbol table
// (including local symbols, scoped by STT_FILE) and its DWARF compile units.
#pragma once

#include "host/elf/elf_file.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::image {

struct ImageSymbol {
    std::string name;
    std::string file; // STT_FILE scope; meaningful for local symbols
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t bind = 0, type = 0, visibility = 0;
    uint32_t shndx = 0;
};

class ElfImage {
public:
    static std::unique_ptr<ElfImage> load(const std::filesystem::path& path);

    const elf::ElfFile& file() const { return file_; }
    uint16_t type() const { return file_.header().type; }
    const std::vector<ImageSymbol>& symbols() const { return symbols_; }
    const std::vector<uint8_t>& build_id() const { return build_id_; }
    // Absolute source paths of the DWARF compile units; empty without debug info.
    const std::vector<std::filesystem::path>& compile_units() const { return compile_units_; }
    const std::string& dwarf_warning() const { return dwarf_warning_; }

    bool section_writable(uint32_t shndx) const;
    // Copies `n` bytes at link-time virtual address `vaddr` from the file. False if
    // the range is not fully file-backed.
    bool read(uint64_t vaddr, size_t n, uint8_t* out) const;

private:
    elf::ElfFile file_;
    std::vector<ImageSymbol> symbols_;
    std::vector<uint8_t> build_id_;
    std::vector<std::filesystem::path> compile_units_;
    std::string dwarf_warning_;
};

// Source paths (DW_AT_comp_dir joined with DW_AT_name) of every DWARF 4/5 compile
// unit in `file`. Throws elf::ElfError on malformed debug info.
std::vector<std::filesystem::path> dwarf_compile_units(const elf::ElfFile& file);

} // namespace hotpp::image
