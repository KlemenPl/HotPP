#include "host/object/elf_object.h"

namespace hotpp::object {

using namespace hotpp::elf;

std::unique_ptr<ElfObject> ElfObject::load(const std::filesystem::path& path) {
    return from_file(ElfFile::load(path));
}

std::unique_ptr<ElfObject> ElfObject::from_file(ElfFile file) {
    auto obj = std::unique_ptr<ElfObject>(new ElfObject);
    obj->file_ = std::move(file);
    const ElfFile& f = obj->file_;
    obj->path = f.name();
    if (f.header().type != ET_REL) throw ElfError(f.name() + ": not a relocatable object");

    const auto& shdrs = f.sections();
    obj->sections.resize(shdrs.size());
    int symtab = -1;
    for (size_t i = 0; i < shdrs.size(); ++i) {
        const Shdr& s = shdrs[i];
        ObjSection& out = obj->sections[i];
        out.name = f.section_name(i);
        out.type = s.type;
        out.flags = s.flags;
        out.size = s.size;
        out.align = s.addralign ? s.addralign : 1;
        if ((out.align & (out.align - 1)) || out.align > (uint64_t(1) << 30))
            throw ElfError(f.name() + ": section " + out.name + " has a bad alignment");
        if (s.type != SHT_NOBITS && s.type != SHT_NULL) out.data = f.section_data(i);
        if (s.type == SHT_SYMTAB) symtab = int(i);
        if ((s.flags & SHF_COMPRESSED) && (s.flags & SHF_ALLOC))
            throw ElfError(f.name() + ": compressed section " + out.name + " is not supported");
    }
    if (symtab < 0) throw ElfError(f.name() + ": no symbol table");

    std::vector<uint32_t> shndx;
    const std::vector<Sym> syms = f.symbols(size_t(symtab), shndx);
    const size_t strtab = shdrs[size_t(symtab)].link;
    obj->symbols.resize(syms.size());
    for (size_t i = 0; i < syms.size(); ++i) {
        ObjSymbol& s = obj->symbols[i];
        s.name = syms[i].name ? f.string_at(strtab, syms[i].name) : std::string();
        s.value = syms[i].value;
        s.size = syms[i].size;
        s.bind = syms[i].bind();
        s.type = syms[i].type();
        s.visibility = syms[i].visibility();
        s.shndx = shndx[i];
        if (s.type == STT_FILE && obj->source_name.empty()) obj->source_name = s.name;
        if (s.defined() && s.shndx < SHN_LORESERVE) {
            if (s.shndx >= shdrs.size()) throw ElfError(f.name() + ": symbol " + s.name + " has a bad section index");
            if (s.value > shdrs[s.shndx].size && shdrs[s.shndx].type != SHT_NULL)
                throw ElfError(f.name() + ": symbol " + s.name + " lies outside its section");
        }
    }

    for (size_t i = 0; i < shdrs.size(); ++i) {
        const Shdr& s = shdrs[i];
        if (s.type == SHT_RELA) {
            if (s.info >= shdrs.size()) throw ElfError(f.name() + ": bad relocation target");
            if (s.entsize != sizeof(Rela)) throw ElfError(f.name() + ": unexpected relocation size");
            const size_t n = size_t(s.size / sizeof(Rela));
            auto& relocs = obj->sections[s.info].relocs;
            for (size_t k = 0; k < n; ++k) {
                Rela r = f.read<Rela>(s.offset + k * sizeof(Rela));
                if (r.sym() >= obj->symbols.size()) throw ElfError(f.name() + ": bad relocation symbol");
                if (r.offset >= shdrs[s.info].size) throw ElfError(f.name() + ": relocation outside its section");
                relocs.push_back(r);
            }
        } else if (s.type == SHT_REL) {
            throw ElfError(f.name() + ": SHT_REL relocations are not used on x86-64");
        } else if (s.type == SHT_GROUP) {
            ObjGroup g;
            if (s.info < obj->symbols.size()) g.signature = obj->symbols[s.info].name;
            const size_t words = size_t(s.size / 4);
            if (words == 0) continue;
            g.flags = f.read<uint32_t>(s.offset);
            for (size_t k = 1; k < words; ++k) {
                uint32_t member = f.read<uint32_t>(s.offset + k * 4);
                if (member >= shdrs.size()) throw ElfError(f.name() + ": bad group member");
                g.members.push_back(member);
                obj->sections[member].group = int(obj->groups.size());
            }
            obj->groups.push_back(std::move(g));
        }
    }
    return obj;
}

} // namespace hotpp::object
