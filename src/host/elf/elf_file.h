// Bounds-checked reading of 64-bit little-endian ELF files. Own definitions instead
// of <elf.h>, so nothing depends on the system headers. Malformed input throws ElfError.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace hotpp::elf {

struct ElfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// clang-format off
constexpr uint16_t ET_REL = 1, ET_EXEC = 2, ET_DYN = 3;
constexpr uint16_t EM_X86_64 = 62;

constexpr uint32_t SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4,
                   SHT_NOTE = 7, SHT_NOBITS = 8, SHT_REL = 9, SHT_DYNSYM = 11,
                   SHT_INIT_ARRAY = 14, SHT_FINI_ARRAY = 15, SHT_PREINIT_ARRAY = 16,
                   SHT_GROUP = 17, SHT_SYMTAB_SHNDX = 18, SHT_X86_64_UNWIND = 0x70000001;
constexpr uint64_t SHF_WRITE = 0x1, SHF_ALLOC = 0x2, SHF_EXECINSTR = 0x4, SHF_MERGE = 0x10,
                   SHF_STRINGS = 0x20, SHF_GROUP = 0x200, SHF_TLS = 0x400, SHF_COMPRESSED = 0x800;
constexpr uint16_t SHN_UNDEF = 0, SHN_LORESERVE = 0xff00, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2,
                   SHN_XINDEX = 0xffff;
constexpr uint8_t STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2, STB_GNU_UNIQUE = 10;
constexpr uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4,
                  STT_COMMON = 5, STT_TLS = 6, STT_GNU_IFUNC = 10;
constexpr uint8_t STV_DEFAULT = 0, STV_INTERNAL = 1, STV_HIDDEN = 2, STV_PROTECTED = 3;
constexpr uint32_t GRP_COMDAT = 1;
constexpr uint32_t PT_LOAD = 1, PT_NOTE = 4, PT_TLS = 7;
constexpr uint32_t NT_GNU_BUILD_ID = 3;

constexpr uint32_t R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_GOT32 = 3,
                   R_X86_64_PLT32 = 4, R_X86_64_GOTPCREL = 9, R_X86_64_32 = 10,
                   R_X86_64_32S = 11, R_X86_64_16 = 12, R_X86_64_PC16 = 13, R_X86_64_8 = 14,
                   R_X86_64_PC8 = 15, R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17,
                   R_X86_64_TPOFF64 = 18, R_X86_64_TLSGD = 19, R_X86_64_TLSLD = 20,
                   R_X86_64_DTPOFF32 = 21, R_X86_64_GOTTPOFF = 22, R_X86_64_TPOFF32 = 23,
                   R_X86_64_PC64 = 24, R_X86_64_GOTOFF64 = 25, R_X86_64_GOTPC32 = 26,
                   R_X86_64_SIZE32 = 32, R_X86_64_SIZE64 = 33, R_X86_64_GOTPC32_TLSDESC = 34,
                   R_X86_64_TLSDESC_CALL = 35, R_X86_64_GOTPCRELX = 41,
                   R_X86_64_REX_GOTPCRELX = 42;
// Relocation types of COFF objects (object/coff_object.cpp) that ELF has no
// equivalent for. COFF REL32 / REL32_k / ADDR32 / ADDR64 map onto the ELF types.
constexpr uint32_t R_HOTPP_RVA32 = 0x10001;    // ADDR32NB: S + A - the image base
constexpr uint32_t R_HOTPP_SECREL32 = 0x10002; // SECREL: offset of S + A in its section (TLS: in the TLS block)
constexpr uint32_t R_HOTPP_SECTION16 = 0x10003; // SECTION: the section number of S (debug info only)
// clang-format on

struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};

struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
    uint8_t bind() const { return info >> 4; }
    uint8_t type() const { return info & 0xf; }
    uint8_t visibility() const { return other & 0x3; }
};

struct Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
    uint32_t sym() const { return uint32_t(info >> 32); }
    uint32_t type() const { return uint32_t(info & 0xffffffff); }
};

static_assert(sizeof(Ehdr) == 64 && sizeof(Phdr) == 56 && sizeof(Shdr) == 64 &&
              sizeof(Sym) == 24 && sizeof(Rela) == 24);

// A whole ELF file in memory with its section headers decoded.
class ElfFile {
public:
    static ElfFile load(const std::filesystem::path& path);
    static ElfFile from_bytes(std::vector<uint8_t> bytes, std::string name);

    const std::string& name() const { return name_; }
    const Ehdr& header() const { return ehdr_; }
    const std::vector<Shdr>& sections() const { return shdrs_; }
    const std::vector<Phdr>& segments() const { return phdrs_; }
    std::string section_name(size_t index) const;
    int find_section(const std::string& name) const; // -1 if absent

    // File bytes of a section (empty for SHT_NOBITS).
    const uint8_t* section_data(size_t index) const;
    // NUL-terminated string at `offset` in string-table section `strtab`.
    std::string string_at(size_t strtab, uint64_t offset) const;
    // Symbols of a SHT_SYMTAB/SHT_DYNSYM section, with SHN_XINDEX resolved into
    // `shndx` (which then holds the real section index).
    std::vector<Sym> symbols(size_t symtab, std::vector<uint32_t>& shndx) const;

    const std::vector<uint8_t>& bytes() const { return bytes_; }

    template <class T>
    T read(uint64_t offset) const {
        check(offset, sizeof(T));
        T v;
        std::memcpy(&v, bytes_.data() + offset, sizeof(T));
        return v;
    }
    void check(uint64_t offset, uint64_t size) const;

private:
    void parse();

    std::string name_;
    std::vector<uint8_t> bytes_;
    Ehdr ehdr_{};
    std::vector<Shdr> shdrs_;
    std::vector<Phdr> phdrs_;
};

// GNU build ID from the file's PT_NOTE segments or .note.gnu.build-id; empty if none.
std::vector<uint8_t> build_id(const ElfFile& file);

} // namespace hotpp::elf
