// A relocatable object as produced by a patch compile: ELF (.o), or COFF (.obj),
// which object/coff_object.cpp translates into the same ELF-shaped structures
// (sections, symbols, RELA relocations, COMDAT groups) the patch linker reads.
#pragma once

#include "host/elf/elf_file.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hotpp::object {

struct ObjSection {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t size = 0;
    uint64_t align = 1;
    const uint8_t* data = nullptr; // nullptr for SHT_NOBITS
    std::vector<elf::Rela> relocs; // relocations applying to this section
    int group = -1;                // index into ElfObject::groups
};

struct ObjSymbol {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t bind = 0, type = 0, visibility = 0;
    uint32_t shndx = 0;
    uint32_t alias = UINT32_MAX; // COFF weak external: the symbol to use if this one stays undefined

    bool defined() const { return shndx != elf::SHN_UNDEF && shndx != elf::SHN_COMMON; }
};

struct ObjGroup {
    std::string signature;
    uint32_t flags = 0;
    std::vector<uint32_t> members; // section indices
};

enum class Format { Elf, Coff };

class ElfObject {
public:
    static std::unique_ptr<ElfObject> load(const std::filesystem::path& path);
    static std::unique_ptr<ElfObject> from_file(elf::ElfFile file);
    // COFF (object/coff_object.cpp). Throws elf::ElfError on malformed input.
    static std::unique_ptr<ElfObject> load_coff(const std::filesystem::path& path);
    static std::unique_ptr<ElfObject> from_coff(std::vector<uint8_t> bytes, std::string name);

    Format format = Format::Elf;
    // COFF: the CodeView type records of .debug$T (struct layouts for warnings).
    std::vector<std::pair<const uint8_t*, size_t>> codeview_types;

    std::string path;
    std::string source_name; // first STT_FILE symbol; scopes this object's local symbols
    std::vector<ObjSection> sections; // indexed like the ELF section header table
    std::vector<ObjSymbol> symbols;   // indexed like the ELF symbol table
    std::vector<ObjGroup> groups;

    const elf::ElfFile& file() const { return file_; }

private:
    elf::ElfFile file_;
    std::vector<uint8_t> bytes_; // COFF: the file
};

// An ELF or COFF object, by its first bytes.
std::unique_ptr<ElfObject> load_object(const std::filesystem::path& path);

} // namespace hotpp::object
