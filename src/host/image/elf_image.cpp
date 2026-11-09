#include "host/image/elf_image.h"

#include "host/image/pe_image.h"

#include <fstream>

namespace hotpp::image {

using namespace hotpp::elf;

std::unique_ptr<ElfImage> ElfImage::load(const std::filesystem::path& path) {
    return from_file(ElfFile::load(path));
}

std::unique_ptr<ElfImage> ElfImage::from_file(ElfFile elf) {
    auto img = std::make_unique<ElfImage>();
    img->file_ = std::move(elf);
    const ElfFile& f = img->file_;
    const std::string path = f.name();
    if (f.header().type != ET_EXEC && f.header().type != ET_DYN)
        throw ElfError(path + ": not an executable");

    int symtab = -1;
    for (size_t i = 0; i < f.sections().size(); ++i)
        if (f.sections()[i].type == SHT_SYMTAB) symtab = int(i);
    if (symtab < 0)
        throw ElfError(path + " has no symbol table (stripped?); HotPP needs an unstripped binary");

    std::vector<uint32_t> shndx;
    const std::vector<Sym> syms = f.symbols(size_t(symtab), shndx);
    const size_t strtab = f.sections()[size_t(symtab)].link;
    std::string file;
    img->symbols_.reserve(syms.size());
    for (size_t i = 0; i < syms.size(); ++i) {
        const Sym& s = syms[i];
        std::string name = s.name ? f.string_at(strtab, s.name) : std::string();
        if (s.type() == STT_FILE) {
            file = name;
            continue;
        }
        if (s.type() == STT_SECTION || name.empty() || shndx[i] == SHN_UNDEF) continue;
        ImageSymbol out;
        out.name = std::move(name);
        if (s.bind() == STB_LOCAL) out.file = file;
        out.value = s.value;
        out.size = s.size;
        out.bind = s.bind();
        out.type = s.type();
        out.visibility = s.visibility();
        out.shndx = shndx[i];
        img->symbols_.push_back(std::move(out));
    }

    img->build_id_ = elf::build_id(f);
    try {
        img->compile_units_ = dwarf_compile_units(f);
    } catch (const ElfError& e) {
        img->dwarf_warning_ = e.what();
    }
    return img;
}

std::unordered_map<std::string, TypeLayout> ElfImage::type_layouts() const { return dwarf_type_layouts(file_); }

std::unique_ptr<ModuleImage> load_module_image(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[2] = {};
    in.read(magic, 2);
    if (magic[0] == 'M' && magic[1] == 'Z') return PeImage::load(path);
    return ElfImage::load(path);
}

bool ElfImage::section_writable(uint32_t shndx) const {
    return shndx < file_.sections().size() && (file_.sections()[shndx].flags & SHF_WRITE);
}

bool ElfImage::read(uint64_t vaddr, size_t n, uint8_t* out) const {
    for (const Shdr& s : file_.sections()) {
        if (!(s.flags & SHF_ALLOC) || s.type == SHT_NOBITS) continue;
        if (vaddr >= s.addr && n <= s.size && vaddr - s.addr <= s.size - n) {
            std::memcpy(out, file_.bytes().data() + s.offset + (vaddr - s.addr), n);
            return true;
        }
    }
    return false;
}

} // namespace hotpp::image
