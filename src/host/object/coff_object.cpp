// COFF objects (MSVC, clang-cl), regular and /bigobj, translated into the ELF-shaped
// structures the patch linker reads:
// - sections get ELF flags and types: .CRT$XCU is the initializer array, .pdata the
//   unwind table, .debug$* and linker directives aren't allocated
// - symbol table indices are kept (aux records become empty symbols) so relocations
//   need no renumbering; section numbers are ELF section indices
// - COMDAT sections with their associative sections (unwind data) form groups
// - relocations get their in-place addend moved into the RELA addend; REL32_k
//   becomes PC32 with addend A - 4 - k
// - static symbols are renamed to bridge keys from their CodeView records
//   (codeview.h), which is how they are found in the PDB
#include "host/object/elf_object.h"

#include "host/image/codeview.h"

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace hotpp::object {

using namespace hotpp::elf;

namespace {

// clang-format off
constexpr uint32_t IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x80,
                   IMAGE_SCN_LNK_INFO = 0x200, IMAGE_SCN_LNK_REMOVE = 0x800, IMAGE_SCN_LNK_COMDAT = 0x1000,
                   IMAGE_SCN_LNK_NRELOC_OVFL = 0x01000000, IMAGE_SCN_MEM_DISCARDABLE = 0x02000000,
                   IMAGE_SCN_MEM_EXECUTE = 0x20000000, IMAGE_SCN_MEM_WRITE = 0x80000000;
constexpr uint8_t IMAGE_SYM_CLASS_EXTERNAL = 2, IMAGE_SYM_CLASS_STATIC = 3, IMAGE_SYM_CLASS_LABEL = 6,
                  IMAGE_SYM_CLASS_FILE = 103, IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105;
constexpr uint8_t IMAGE_COMDAT_SELECT_NODUPLICATES = 1, IMAGE_COMDAT_SELECT_ASSOCIATIVE = 5;
constexpr uint16_t IMAGE_REL_AMD64_ABSOLUTE = 0, IMAGE_REL_AMD64_ADDR64 = 1, IMAGE_REL_AMD64_ADDR32 = 2,
                   IMAGE_REL_AMD64_ADDR32NB = 3, IMAGE_REL_AMD64_REL32 = 4, IMAGE_REL_AMD64_REL32_5 = 9,
                   IMAGE_REL_AMD64_SECTION = 10, IMAGE_REL_AMD64_SECREL = 11;
constexpr uint8_t kBigObjClassId[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                        0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};
// clang-format on

class Reader {
public:
    Reader(const std::vector<uint8_t>& b, const std::string& name) : b_(b), name_(name) {}
    template <class T>
    T at(uint64_t off) const {
        if (off > b_.size() || sizeof(T) > b_.size() - off) fail("truncated");
        T v;
        std::memcpy(&v, b_.data() + off, sizeof v);
        return v;
    }
    const uint8_t* ptr(uint64_t off, uint64_t n) const {
        if (off > b_.size() || n > b_.size() - off) fail("data outside the file");
        return b_.data() + off;
    }
    [[noreturn]] void fail(const std::string& what) const { throw ElfError(name_ + ": " + what); }

private:
    const std::vector<uint8_t>& b_;
    const std::string& name_;
};

std::string basename(const std::string& p) {
    const size_t sep = p.find_last_of("/\\");
    return sep == std::string::npos ? p : p.substr(sep + 1);
}

// Linker directives: /alternatename:A=B (A falls back to B, like a weak alias).
std::map<std::string, std::string> alternate_names(const uint8_t* p, size_t n) {
    std::map<std::string, std::string> out;
    std::string text(reinterpret_cast<const char*>(p), n);
    std::istringstream in(text);
    for (std::string tok; in >> tok;) {
        if (!tok.empty() && tok.front() == '"' && tok.back() == '"' && tok.size() >= 2) tok = tok.substr(1, tok.size() - 2);
        for (const char* flag : {"/alternatename:", "-alternatename:", "/ALTERNATENAME:", "-ALTERNATENAME:"}) {
            if (tok.rfind(flag, 0) != 0) continue;
            const std::string pair = tok.substr(std::strlen(flag));
            const size_t eq = pair.find('=');
            if (eq != std::string::npos) out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
    return out;
}

} // namespace

std::unique_ptr<ElfObject> ElfObject::load_coff(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ElfError("cannot read " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return from_coff(std::move(bytes), path.string());
}

std::unique_ptr<ElfObject> ElfObject::from_coff(std::vector<uint8_t> bytes, std::string name) {
    auto obj = std::unique_ptr<ElfObject>(new ElfObject);
    obj->bytes_ = std::move(bytes);
    obj->format = Format::Coff;
    obj->path = name;
    const Reader r(obj->bytes_, obj->path);

    // Header: regular or /bigobj.
    bool big = false;
    uint32_t nsections = 0, symtab = 0, nsyms = 0;
    uint64_t section_table = 0;
    if (r.at<uint16_t>(0) == 0 && r.at<uint16_t>(2) == 0xFFFF) {
        if (std::memcmp(r.ptr(12, 16), kBigObjClassId, 16) != 0) r.fail("not a COFF object");
        if (r.at<uint16_t>(6) != 0x8664) r.fail("not an x86-64 object");
        big = true;
        nsections = r.at<uint32_t>(44);
        symtab = r.at<uint32_t>(48);
        nsyms = r.at<uint32_t>(52);
        section_table = 56;
    } else {
        if (r.at<uint16_t>(0) != 0x8664) r.fail("not an x86-64 COFF object");
        nsections = r.at<uint16_t>(2);
        symtab = r.at<uint32_t>(8);
        nsyms = r.at<uint32_t>(12);
        section_table = 20 + r.at<uint16_t>(16);
    }
    if (nsections > 1u << 24 || nsyms > 1u << 26) r.fail("too many sections or symbols");
    const size_t symsize = big ? 20 : 18;
    const uint64_t strtab = uint64_t(symtab) + uint64_t(nsyms) * symsize;
    const uint32_t strtab_size = symtab ? r.at<uint32_t>(strtab) : 0;
    auto string_at = [&](uint32_t off) -> std::string {
        if (off < 4 || off >= strtab_size) r.fail("bad string table offset");
        const uint8_t* p = r.ptr(strtab + off, strtab_size - off);
        return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), strtab_size - off));
    };
    auto short_name = [&](const uint8_t* p) {
        return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 8));
    };

    // Sections: index i + 1 is COFF section number i + 1; 0 stays empty.
    obj->sections.resize(size_t(nsections) + 1);
    struct Raw {
        uint32_t characteristics = 0, relocs_at = 0, nrelocs = 0;
    };
    std::vector<Raw> raw(nsections + 1);
    for (uint32_t i = 0; i < nsections; ++i) {
        const uint64_t h = section_table + uint64_t(i) * 40;
        const uint8_t* np = r.ptr(h, 8);
        std::string sname = short_name(np);
        if (!sname.empty() && sname[0] == '/') sname = string_at(uint32_t(std::strtoul(sname.c_str() + 1, nullptr, 10)));
        const uint32_t raw_size = r.at<uint32_t>(h + 16), raw_ptr = r.at<uint32_t>(h + 20);
        Raw& rw = raw[i + 1];
        rw.relocs_at = r.at<uint32_t>(h + 24);
        rw.nrelocs = r.at<uint16_t>(h + 32);
        rw.characteristics = r.at<uint32_t>(h + 36);
        if ((rw.characteristics & IMAGE_SCN_LNK_NRELOC_OVFL) && rw.nrelocs == 0xFFFF)
            rw.nrelocs = r.at<uint32_t>(rw.relocs_at) - 1, rw.relocs_at += 10;

        ObjSection& s = obj->sections[i + 1];
        s.name = sname;
        const uint32_t ch = rw.characteristics;
        const uint32_t align_code = (ch >> 20) & 0xF;
        s.align = align_code ? uint64_t(1) << (align_code - 1) : 16;
        s.size = raw_size;
        const bool debug = sname.rfind(".debug$", 0) == 0;
        const bool info = (ch & (IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE)) || (ch & IMAGE_SCN_MEM_DISCARDABLE) || debug;
        // Sections the patch never needs: control-flow-guard tables, symbol-check
        // data, address-significance lists.
        const bool unused = sname == ".chks64" || sname == ".sxdata" || sname == ".voltbl" || sname.rfind(".gfids", 0) == 0 ||
                            sname.rfind(".giats", 0) == 0 || sname.rfind(".gljmp", 0) == 0 ||
                            sname.rfind(".gehcont", 0) == 0 || sname.rfind(".llvm_", 0) == 0 || sname.rfind(".llvm.", 0) == 0;
        if (!info && !unused) s.flags |= SHF_ALLOC;
        if (ch & IMAGE_SCN_MEM_EXECUTE) s.flags |= SHF_EXECINSTR;
        if (ch & IMAGE_SCN_MEM_WRITE) s.flags |= SHF_WRITE;
        if (sname.rfind(".tls", 0) == 0) s.flags |= SHF_TLS | SHF_WRITE;
        s.type = SHT_PROGBITS;
        if (ch & IMAGE_SCN_CNT_UNINITIALIZED_DATA) {
            s.type = SHT_NOBITS;
        } else if (raw_size) {
            s.data = r.ptr(raw_ptr, raw_size);
        }
        if (sname == ".pdata") s.type = SHT_X86_64_UNWIND;
        // C++ dynamic initializers. The other .CRT$X* lists (C initializers, TLS
        // callbacks, terminators) are left out, like .fini_array on ELF.
        if (sname.rfind(".CRT$XC", 0) == 0 && sname != ".CRT$XCA" && sname != ".CRT$XCZ") {
            s.type = SHT_INIT_ARRAY;
        } else if (sname.rfind(".CRT$", 0) == 0) {
            s.flags &= ~SHF_ALLOC;
        }
        if (sname == ".debug$T" && s.data && s.size >= 4) {
            uint32_t sig;
            std::memcpy(&sig, s.data, 4);
            if (sig == codeview::kSignatureC13) obj->codeview_types.emplace_back(s.data + 4, size_t(s.size - 4));
        }
    }

    // Symbols, aux records included.
    obj->symbols.resize(nsyms);
    struct ComdatInfo {
        uint8_t selection = 0;
        uint32_t associated = 0; // ASSOCIATIVE: the section this one goes with
        int leader = -1;         // the COMDAT symbol
    };
    std::vector<ComdatInfo> comdat(nsections + 1);
    for (uint32_t i = 0; i < nsyms; ++i) {
        const uint64_t at = symtab + uint64_t(i) * symsize;
        const uint8_t* np = r.ptr(at, 8);
        std::string sname = r.at<uint32_t>(at) == 0 ? string_at(r.at<uint32_t>(at + 4)) : short_name(np);
        const uint32_t value = r.at<uint32_t>(at + 8);
        const int32_t secnum = big ? r.at<int32_t>(at + 12) : int32_t(r.at<int16_t>(at + 12));
        const uint16_t type = r.at<uint16_t>(at + (big ? 16 : 14));
        const uint8_t sclass = r.at<uint8_t>(at + (big ? 18 : 16));
        const uint8_t naux = r.at<uint8_t>(at + (big ? 19 : 17));
        if (uint64_t(i) + naux >= nsyms && naux) r.fail("truncated symbol table");
        const uint64_t aux = at + symsize;

        ObjSymbol& s = obj->symbols[i];
        s.value = value;
        s.bind = STB_LOCAL;
        s.type = STT_NOTYPE;
        if (secnum > 0) {
            if (uint32_t(secnum) > nsections) r.fail("symbol " + sname + " has a bad section number");
            s.shndx = uint32_t(secnum);
        } else if (secnum == -1) {
            s.shndx = SHN_ABS;
        } else {
            s.shndx = SHN_UNDEF;
        }
        const bool in_tls = s.shndx && s.shndx <= nsections && (obj->sections[s.shndx].flags & SHF_TLS);
        const bool function = ((type >> 4) & 3) == 2;

        switch (sclass) {
        case IMAGE_SYM_CLASS_EXTERNAL:
            s.name = sname;
            s.bind = STB_GLOBAL;
            if (secnum == 0 && value) { // common
                s.shndx = SHN_COMMON;
                s.size = value;
                s.value = 0;
            }
            s.type = function ? STT_FUNC : s.shndx && s.shndx != SHN_ABS ? (in_tls ? STT_TLS : STT_OBJECT) : STT_NOTYPE;
            break;
        case IMAGE_SYM_CLASS_WEAK_EXTERNAL:
            s.name = sname;
            s.bind = STB_WEAK;
            s.shndx = SHN_UNDEF;
            if (naux) s.alias = r.at<uint32_t>(aux);
            break;
        case IMAGE_SYM_CLASS_STATIC:
            if (naux && value == 0 && secnum > 0 && sname == obj->sections[size_t(secnum)].name) {
                // A section symbol, with the section's COMDAT selection.
                s.type = STT_SECTION;
                s.name = sname;
                ComdatInfo& c = comdat[size_t(secnum)];
                c.selection = r.at<uint8_t>(aux + 14);
                c.associated = r.at<uint16_t>(aux + 12) | (big ? uint32_t(r.at<uint16_t>(aux + 16)) << 16 : 0);
            } else {
                s.name = sname;
                s.type = function ? STT_FUNC : in_tls ? STT_TLS : (s.shndx && s.shndx != SHN_ABS ? STT_OBJECT : STT_NOTYPE);
            }
            break;
        case IMAGE_SYM_CLASS_LABEL:
            s.name = sname; // $LN labels: local, never named for the linker (see below)
            break;
        case IMAGE_SYM_CLASS_FILE: {
            s.type = STT_FILE;
            std::string file(reinterpret_cast<const char*>(r.ptr(aux, uint64_t(naux) * symsize)), size_t(naux) * symsize);
            file.resize(strnlen(file.c_str(), file.size()));
            s.name = file;
            if (obj->source_name.empty()) obj->source_name = basename(file);
            break;
        }
        default: // FUNCTION (.bf/.ef), SECTION and others: not needed
            s.shndx = SHN_UNDEF;
            break;
        }
        // Compiler-internal labels ($LN, $unwind$, $pdata$...) are scoped like ELF's .L.
        if (s.type != STT_SECTION && s.type != STT_FILE && !s.name.empty() && s.name[0] == '$') s.name = ".L" + s.name;
        // The COMDAT symbol: the first named symbol in the section.
        if (s.shndx && s.shndx <= nsections && s.type != STT_SECTION && comdat[s.shndx].leader < 0 && !s.name.empty() &&
            s.name.rfind(".L", 0) != 0)
            comdat[s.shndx].leader = int(i);
        for (uint8_t k = 0; k < naux; ++k) {
            ObjSymbol& a = obj->symbols[i + 1 + k];
            a = ObjSymbol{}; // aux slot
        }
        i += naux;
    }
    for (ObjSymbol& s : obj->symbols) {
        if (s.alias != UINT32_MAX && s.alias >= nsyms) r.fail("weak external with a bad alias");
        // Throwing from patch code goes through the agent: _CxxThrowException finds the
        // throw information's image with RtlPcToFileHeader, which fails for patch
        // memory (docs/DESIGN.md §13, Windows spike).
        if (s.name == "_CxxThrowException" && !s.defined()) s.name = "__hotpp_cxx_throw";
    }

    // /alternatename directives become aliases of the undefined symbols they name.
    for (uint32_t i = 1; i <= nsections; ++i) {
        const ObjSection& s = obj->sections[i];
        if (s.name != ".drectve" || !s.data) continue;
        for (const auto& [from, to] : alternate_names(s.data, size_t(s.size))) {
            uint32_t from_index = UINT32_MAX, to_index = UINT32_MAX;
            for (uint32_t k = 0; k < obj->symbols.size(); ++k) {
                if (obj->symbols[k].name == from && !obj->symbols[k].defined()) from_index = k;
                if (obj->symbols[k].name == to && obj->symbols[k].bind != STB_LOCAL) to_index = k;
            }
            if (from_index == UINT32_MAX) continue;
            if (to_index == UINT32_MAX) {
                ObjSymbol target;
                target.name = to;
                target.bind = STB_GLOBAL;
                to_index = uint32_t(obj->symbols.size());
                obj->symbols.push_back(target);
            }
            obj->symbols[from_index].bind = STB_WEAK;
            obj->symbols[from_index].alias = to_index;
        }
    }

    // COMDAT groups: a section with its associative sections, deduplicated across
    // the patch's objects by the COMDAT symbol's name. Sections whose symbol is
    // static, or that must not have duplicates, need no deduplication.
    std::map<uint32_t, int> group_of;
    for (uint32_t i = 1; i <= nsections; ++i) {
        if (!(raw[i].characteristics & IMAGE_SCN_LNK_COMDAT)) continue;
        const ComdatInfo& c = comdat[i];
        if (c.selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE || c.leader < 0) continue;
        const ObjSymbol& lead = obj->symbols[size_t(c.leader)];
        ObjGroup g;
        g.signature = lead.name;
        g.flags = lead.bind == STB_LOCAL || c.selection == IMAGE_COMDAT_SELECT_NODUPLICATES ? 0 : GRP_COMDAT;
        g.members.push_back(i);
        group_of[i] = int(obj->groups.size());
        obj->sections[i].group = int(obj->groups.size());
        obj->groups.push_back(std::move(g));
    }
    for (uint32_t i = 1; i <= nsections; ++i) {
        const ComdatInfo& c = comdat[i];
        if (!(raw[i].characteristics & IMAGE_SCN_LNK_COMDAT) || c.selection != IMAGE_COMDAT_SELECT_ASSOCIATIVE) continue;
        auto it = group_of.find(c.associated);
        if (it == group_of.end()) continue;
        obj->groups[size_t(it->second)].members.push_back(i);
        obj->sections[i].group = it->second;
    }

    // Symbol sizes (COFF has none): a symbol alone at the start of its section (one
    // function or variable per section with /Gy /Gw) has the section's size.
    std::vector<int> named_in(nsections + 1, 0);
    for (const ObjSymbol& s : obj->symbols)
        if (s.shndx && s.shndx <= nsections && s.type != STT_SECTION && !s.name.empty() && s.name.rfind(".L", 0) != 0)
            ++named_in[s.shndx];
    for (ObjSymbol& s : obj->symbols)
        if (s.shndx && s.shndx <= nsections && s.type != STT_SECTION && s.value == 0 && named_in[s.shndx] == 1 && !s.size)
            s.size = obj->sections[s.shndx].size;

    // Relocations.
    for (uint32_t i = 1; i <= nsections; ++i) {
        ObjSection& s = obj->sections[i];
        for (uint32_t k = 0; k < raw[i].nrelocs; ++k) {
            const uint64_t at = uint64_t(raw[i].relocs_at) + uint64_t(k) * 10;
            const uint32_t off = r.at<uint32_t>(at);
            const uint32_t sym = r.at<uint32_t>(at + 4);
            const uint16_t type = r.at<uint16_t>(at + 8);
            if (sym >= obj->symbols.size()) r.fail("bad relocation symbol");
            if (type == IMAGE_REL_AMD64_ABSOLUTE) continue;
            const size_t width = type == IMAGE_REL_AMD64_ADDR64 ? 8 : type == IMAGE_REL_AMD64_SECTION ? 2 : 4;
            if (!s.data || off > s.size || width > s.size - off) r.fail("relocation outside section " + s.name);
            int64_t inplace = 0;
            if (width == 8) {
                int64_t v;
                std::memcpy(&v, s.data + off, 8);
                inplace = v;
            } else if (width == 4) {
                int32_t v;
                std::memcpy(&v, s.data + off, 4);
                inplace = v;
            }
            uint32_t elf_type;
            int64_t addend = inplace;
            if (type == IMAGE_REL_AMD64_ADDR64) {
                elf_type = R_X86_64_64;
            } else if (type == IMAGE_REL_AMD64_ADDR32) {
                elf_type = R_X86_64_32;
                addend = int64_t(uint32_t(inplace));
            } else if (type == IMAGE_REL_AMD64_ADDR32NB) {
                elf_type = R_HOTPP_RVA32;
            } else if (type >= IMAGE_REL_AMD64_REL32 && type <= IMAGE_REL_AMD64_REL32_5) {
                elf_type = R_X86_64_PC32;
                addend = inplace - 4 - int64_t(type - IMAGE_REL_AMD64_REL32);
            } else if (type == IMAGE_REL_AMD64_SECREL) {
                elf_type = R_HOTPP_SECREL32;
            } else if (type == IMAGE_REL_AMD64_SECTION) {
                elf_type = R_HOTPP_SECTION16;
                addend = 0;
            } else {
                elf_type = 0x20000u + type; // unsupported; an error if it's in a loaded section
            }
            Rela rela;
            rela.offset = off;
            rela.info = (uint64_t(sym) << 32) | elf_type;
            rela.addend = addend;
            s.relocs.push_back(rela);
        }
    }

    // Statics are renamed to the keys their CodeView records give them, so they can
    // be found in the PDB. Each record's offset field carries a relocation to its symbol.
    codeview::TypeTable types;
    if (!obj->codeview_types.empty())
        types = codeview::TypeTable::parse(obj->codeview_types[0].first, obj->codeview_types[0].second);
    for (uint32_t i = 1; i <= nsections; ++i) {
        const ObjSection& s = obj->sections[i];
        if (s.name != ".debug$S" || !s.data || s.size < 4) continue;
        uint32_t sig;
        std::memcpy(&sig, s.data, 4);
        if (sig != codeview::kSignatureC13) continue;
        std::map<uint64_t, uint32_t> reloc_sym; // offset -> symbol of its SECREL relocation
        for (const Rela& rel : s.relocs)
            if (rel.type() == R_HOTPP_SECREL32) reloc_sym[rel.offset] = rel.sym();
        std::vector<codeview::NamedSymbol> named;
        for (size_t pos = 4; pos + 8 <= s.size;) {
            uint32_t sub, len;
            std::memcpy(&sub, s.data + pos, 4);
            std::memcpy(&len, s.data + pos + 4, 4);
            if (len > s.size - pos - 8) break;
            if (sub == codeview::kDebugSSymbols) {
                try {
                    codeview::collect_symbols(s.data + pos + 8, len, pos + 8, types, &types, named);
                } catch (const codeview::CvError&) {
                    // keep the statics' COFF names: they're then loaded new
                }
            }
            pos += 8 + ((size_t(len) + 3) & ~size_t(3));
        }
        for (const codeview::NamedSymbol& n : named) {
            auto it = reloc_sym.find(n.offset_field);
            if (it == reloc_sym.end()) continue;
            ObjSymbol& target = obj->symbols[it->second];
            // Initializer functions keep their names: the linker recognizes them by those.
            const bool initializer = target.name.rfind("??__E", 0) == 0 || target.name.rfind("??__F", 0) == 0 ||
                                     target.name.rfind("_GLOBAL__sub_I_", 0) == 0;
            if (target.bind == STB_LOCAL && target.type != STT_SECTION && n.local && !initializer) target.name = n.key;
        }
    }
    return obj;
}

std::unique_ptr<ElfObject> load_object(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    if (std::memcmp(magic, "\x7f" "ELF", 4) == 0) return ElfObject::load(path);
    return ElfObject::load_coff(path);
}

} // namespace hotpp::object
