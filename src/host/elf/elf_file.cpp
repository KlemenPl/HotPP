#include "host/elf/elf_file.h"

#include <fstream>

namespace hotpp::elf {

ElfFile ElfFile::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ElfError("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return from_bytes(std::move(bytes), path.string());
}

ElfFile ElfFile::from_bytes(std::vector<uint8_t> bytes, std::string name) {
    ElfFile f;
    f.name_ = std::move(name);
    f.bytes_ = std::move(bytes);
    f.parse();
    return f;
}

void ElfFile::check(uint64_t offset, uint64_t size) const {
    if (offset > bytes_.size() || size > bytes_.size() - offset)
        throw ElfError(name_ + ": truncated or corrupt ELF file");
}

void ElfFile::parse() {
    check(0, sizeof(Ehdr));
    std::memcpy(&ehdr_, bytes_.data(), sizeof ehdr_);
    if (std::memcmp(ehdr_.ident, "\x7f" "ELF", 4) != 0) throw ElfError(name_ + ": not an ELF file");
    if (ehdr_.ident[4] != 2 || ehdr_.ident[5] != 1)
        throw ElfError(name_ + ": only 64-bit little-endian ELF is supported");
    if (ehdr_.machine != EM_X86_64) throw ElfError(name_ + ": not an x86-64 ELF file");

    if (ehdr_.shoff) {
        if (ehdr_.shentsize != sizeof(Shdr)) throw ElfError(name_ + ": unexpected section header size");
        // e_shnum == 0 means the real count lives in section 0's sh_size.
        uint64_t count = ehdr_.shnum;
        if (count == 0) count = read<Shdr>(ehdr_.shoff).size;
        check(ehdr_.shoff, count * sizeof(Shdr));
        shdrs_.resize(count);
        std::memcpy(shdrs_.data(), bytes_.data() + ehdr_.shoff, count * sizeof(Shdr));
    }
    if (ehdr_.phoff && ehdr_.phnum) {
        if (ehdr_.phentsize != sizeof(Phdr)) throw ElfError(name_ + ": unexpected program header size");
        check(ehdr_.phoff, uint64_t(ehdr_.phnum) * sizeof(Phdr));
        phdrs_.resize(ehdr_.phnum);
        std::memcpy(phdrs_.data(), bytes_.data() + ehdr_.phoff, phdrs_.size() * sizeof(Phdr));
    }
    for (const Shdr& s : shdrs_)
        if (s.type != SHT_NOBITS && s.type != SHT_NULL) check(s.offset, s.size);
}

std::string ElfFile::section_name(size_t index) const {
    uint32_t strndx = ehdr_.shstrndx;
    if (strndx == SHN_XINDEX && !shdrs_.empty()) strndx = shdrs_[0].link;
    if (index >= shdrs_.size() || strndx >= shdrs_.size()) return {};
    return string_at(strndx, shdrs_[index].name);
}

int ElfFile::find_section(const std::string& name) const {
    for (size_t i = 0; i < shdrs_.size(); ++i)
        if (section_name(i) == name) return int(i);
    return -1;
}

const uint8_t* ElfFile::section_data(size_t index) const {
    if (index >= shdrs_.size()) throw ElfError(name_ + ": bad section index");
    const Shdr& s = shdrs_[index];
    if (s.type == SHT_NOBITS) return nullptr;
    return bytes_.data() + s.offset;
}

std::string ElfFile::string_at(size_t strtab, uint64_t offset) const {
    if (strtab >= shdrs_.size()) throw ElfError(name_ + ": bad string table index");
    const Shdr& s = shdrs_[strtab];
    if (offset >= s.size) throw ElfError(name_ + ": string offset out of range");
    const char* begin = reinterpret_cast<const char*>(bytes_.data() + s.offset + offset);
    const size_t max = size_t(s.size - offset);
    return std::string(begin, strnlen(begin, max));
}

std::vector<Sym> ElfFile::symbols(size_t symtab, std::vector<uint32_t>& shndx) const {
    const Shdr& s = shdrs_.at(symtab);
    if (s.entsize != sizeof(Sym)) throw ElfError(name_ + ": unexpected symbol size");
    const size_t count = size_t(s.size / sizeof(Sym));
    std::vector<Sym> syms(count);
    if (count) std::memcpy(syms.data(), bytes_.data() + s.offset, count * sizeof(Sym));

    // Extended section indices for files with more than ~65k sections.
    const uint32_t* xindex = nullptr;
    for (size_t i = 0; i < shdrs_.size(); ++i) {
        if (shdrs_[i].type == SHT_SYMTAB_SHNDX && shdrs_[i].link == symtab) {
            if (shdrs_[i].size < count * 4) throw ElfError(name_ + ": short SHT_SYMTAB_SHNDX");
            xindex = reinterpret_cast<const uint32_t*>(bytes_.data() + shdrs_[i].offset);
        }
    }
    shndx.resize(count);
    for (size_t i = 0; i < count; ++i) {
        uint32_t idx = syms[i].shndx;
        if (idx == SHN_XINDEX) {
            if (!xindex) throw ElfError(name_ + ": SHN_XINDEX without SHT_SYMTAB_SHNDX");
            std::memcpy(&idx, xindex + i, 4);
        }
        shndx[i] = idx;
    }
    return syms;
}

namespace {
std::vector<uint8_t> find_build_id(const uint8_t* p, const uint8_t* end) {
    while (p + 12 <= end) {
        uint32_t namesz, descsz, type;
        std::memcpy(&namesz, p, 4);
        std::memcpy(&descsz, p + 4, 4);
        std::memcpy(&type, p + 8, 4);
        const uint8_t* name = p + 12;
        const uint8_t* desc = name + ((namesz + 3) & ~3u);
        const uint8_t* next = desc + ((descsz + 3) & ~3u);
        if (next > end || desc + descsz > end) break;
        if (type == NT_GNU_BUILD_ID && namesz == 4 && std::memcmp(name, "GNU", 4) == 0)
            return std::vector<uint8_t>(desc, desc + descsz);
        p = next;
    }
    return {};
}
} // namespace

std::vector<uint8_t> build_id(const ElfFile& file) {
    const uint8_t* base = file.bytes().data();
    for (const Phdr& ph : file.segments()) {
        if (ph.type != PT_NOTE) continue;
        file.check(ph.offset, ph.filesz);
        auto id = find_build_id(base + ph.offset, base + ph.offset + ph.filesz);
        if (!id.empty()) return id;
    }
    for (size_t i = 0; i < file.sections().size(); ++i) {
        const Shdr& s = file.sections()[i];
        if (s.type != SHT_NOTE) continue;
        auto id = find_build_id(base + s.offset, base + s.offset + s.size);
        if (!id.empty()) return id;
    }
    return {};
}

} // namespace hotpp::elf
