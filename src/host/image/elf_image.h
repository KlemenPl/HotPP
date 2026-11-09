// The running program's executable, read from disk: its full symbol table
// (including local symbols, scoped by STT_FILE) and its DWARF compile units.
#pragma once

#include "host/elf/elf_file.h"
#include "host/image/module_image.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::image {

class ElfImage final : public ModuleImage {
public:
    static std::unique_ptr<ElfImage> load(const std::filesystem::path& path);
    static std::unique_ptr<ElfImage> from_file(elf::ElfFile file);

    const elf::ElfFile& file() const { return file_; }
    uint16_t type() const { return file_.header().type; }
    const std::vector<ImageSymbol>& symbols() const override { return symbols_; }
    const std::vector<uint8_t>& build_id() const override { return build_id_; }
    // Absolute source paths of the DWARF compile units; empty without debug info.
    const std::vector<std::filesystem::path>& compile_units() const override { return compile_units_; }
    const std::string& debug_warning() const override { return dwarf_warning_; }

    bool section_writable(uint32_t shndx) const override;
    // Copies `n` bytes at link-time virtual address `vaddr` from the file. False if
    // the range is not fully file-backed.
    bool read(uint64_t vaddr, size_t n, uint8_t* out) const override;
    SiteStyle site_style() const override { return SiteStyle::PatchableEntry; }
    std::unordered_map<std::string, TypeLayout> type_layouts() const override;

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
