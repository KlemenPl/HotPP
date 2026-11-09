// A PE executable or DLL and its PDB (docs/DESIGN.md §6, Windows): publics give the
// decorated names of external symbols, module symbol streams the statics (under
// their bridge keys, codeview.h), LF_BUILDINFO records the source files, TPI the
// struct layouts. Malformed input throws std::runtime_error.
#pragma once

#include "host/image/module_image.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::image {

class PeImage final : public ModuleImage {
public:
    // `pdb` overrides where the PDB is looked for (by default: the path the image
    // records, then next to the image).
    static std::unique_ptr<PeImage> load(const std::filesystem::path& path, const std::filesystem::path& pdb = {});

    const std::vector<ImageSymbol>& symbols() const override { return symbols_; }
    const std::vector<uint8_t>& build_id() const override { return build_id_; }
    const std::vector<std::filesystem::path>& compile_units() const override { return compile_units_; }
    const std::string& debug_warning() const override { return warning_; }
    bool section_writable(uint32_t shndx) const override;
    bool read(uint64_t rva, size_t n, uint8_t* out) const override;
    SiteStyle site_style() const override { return SiteStyle::Hotpatch; }
    std::unordered_map<std::string, TypeLayout> type_layouts() const override { return layouts_; }

    uint64_t preferred_base() const { return image_base_; }
    const std::filesystem::path& pdb_path() const { return pdb_path_; }

private:
    struct Section {
        std::string name;
        uint32_t rva = 0, virtual_size = 0, raw_offset = 0, raw_size = 0, characteristics = 0;
    };
    void read_pdb(const std::filesystem::path& pdb);
    uint64_t rva_of(uint16_t segment, uint32_t offset) const;

    std::vector<uint8_t> file_;
    std::vector<Section> sections_;
    uint64_t image_base_ = 0;
    uint32_t tls_start_rva_ = 0, tls_end_rva_ = 0; // the TLS template (with its zero fill); 0 without TLS
    std::string pdb_from_image_;
    std::filesystem::path pdb_path_;
    std::vector<ImageSymbol> symbols_;
    std::vector<uint8_t> build_id_;
    std::vector<std::filesystem::path> compile_units_;
    std::string warning_;
    std::unordered_map<std::string, TypeLayout> layouts_;
};

} // namespace hotpp::image
