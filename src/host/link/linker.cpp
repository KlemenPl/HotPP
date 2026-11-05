#include "host/link/linker.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace hotpp::link {

namespace {

using namespace hotpp::elf;
using object::ElfObject;
using object::ObjSection;
using object::ObjSymbol;

enum class SecKind { Skip, Text, ReadOnly, Data, Bss };

struct SecState {
    SecKind kind = SecKind::Skip;
    bool discarded = false; // duplicate COMDAT copy
    bool bound_old = false; // data that stays where the running program has it
    uint64_t old_base = 0;  // bound_old: address of section offset 0 in the process
    uint64_t addr = 0;      // loaded: address in the patch region
};

struct ObjState {
    const ElfObject* obj = nullptr;
    std::vector<SecState> secs;
    std::vector<const IndexEntry*> existing; // per symbol: what it replaces, if anything
};

struct Target {
    enum Kind { Absolute, InPatch, External } kind = Absolute;
    uint64_t value = 0; // Absolute/External: address. InPatch: offset in section
    size_t obj = 0;
    uint32_t sec = 0;
};

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool is_vtable_or_rtti(const std::string& n) {
    return starts_with(n, "_ZTV") || starts_with(n, "_ZTI") || starts_with(n, "_ZTS") ||
           starts_with(n, "_ZTT");
}

bool named(const ObjSymbol& s) {
    return !s.name.empty() && s.type != STT_SECTION && s.type != STT_FILE && !starts_with(s.name, ".L");
}

SymbolKey key_of(const ElfObject& o, const ObjSymbol& s) {
    const bool local = s.bind == STB_LOCAL;
    return {local, local ? o.source_name : std::string(), s.name};
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%" PRIx64, v);
    return buf;
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::string file_of(const ElfObject& o) {
    return o.source_name.empty() ? o.path : o.source_name;
}

SecKind classify(const ObjSection& s, const ElfObject& o, std::vector<std::string>& notes) {
    if (!(s.flags & SHF_ALLOC)) return SecKind::Skip;
    switch (s.type) {
    case SHT_GROUP: case SHT_NOTE: case SHT_X86_64_UNWIND: case SHT_RELA: case SHT_SYMTAB:
    case SHT_STRTAB:
        return SecKind::Skip;
    default: break;
    }
    if (s.type == SHT_INIT_ARRAY || s.type == SHT_FINI_ARRAY || s.type == SHT_PREINIT_ARRAY ||
        starts_with(s.name, ".init_array") || starts_with(s.name, ".fini_array") ||
        starts_with(s.name, ".ctors") || starts_with(s.name, ".dtors")) {
        if (s.size && starts_with(s.name, ".init_array"))
            notes.push_back(file_of(o) + ": dynamic initializers were not run (supported from M3); "
                                         "new globals that need them start zeroed");
        return SecKind::Skip;
    }
    // Unwind tables are registered from M3 on; patch selectors aren't needed.
    if (s.name == ".eh_frame" || s.name == "__patchable_function_entries") return SecKind::Skip;
    if (s.flags & SHF_TLS) {
        if (s.size) throw RestartRequired(file_of(o) + " defines thread_local variables, which are not supported yet (M3)");
        return SecKind::Skip;
    }
    if (s.flags & SHF_EXECINSTR) return SecKind::Text;
    if (s.type == SHT_NOBITS) return SecKind::Bss;
    if (!(s.flags & SHF_WRITE) || starts_with(s.name, ".data.rel.ro")) return SecKind::ReadOnly;
    return SecKind::Data;
}

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

class Linker {
public:
    Linker(const std::vector<const ElfObject*>& objects, const SymbolIndex& index,
           const ExternalResolver& resolve, const LinkOptions& opt)
        : index_(index), resolve_(resolve), opt_(opt) {
        st_.resize(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) {
            st_[i].obj = objects[i];
            st_[i].secs.resize(objects[i]->sections.size());
            st_[i].existing.assign(objects[i]->symbols.size(), nullptr);
        }
    }

    LinkResult run() {
        dedupe_comdats();
        classify_sections();
        find_existing();
        bind_data();
        collect_globals();
        resolve_externals();
        plan_got_and_stubs();
        layout();
        apply_relocations();
        make_sites_and_delta();
        emit_segments();
        return std::move(out_);
    }

private:
    const ElfObject& obj(size_t o) const { return *st_[o].obj; }
    bool loaded(const SecState& s) const { return s.kind != SecKind::Skip && !s.discarded; }

    void dedupe_comdats() {
        std::unordered_set<std::string> seen;
        for (ObjState& os : st_)
            for (const object::ObjGroup& g : os.obj->groups)
                if ((g.flags & GRP_COMDAT) && !seen.insert(g.signature).second)
                    for (uint32_t m : g.members) os.secs[m].discarded = true;
    }

    void classify_sections() {
        for (ObjState& os : st_)
            for (size_t i = 0; i < os.secs.size(); ++i)
                if (!os.secs[i].discarded) os.secs[i].kind = classify(os.obj->sections[i], *os.obj, out_.notes);
    }

    bool in_loaded_section(size_t o, const ObjSymbol& s) const {
        return s.defined() && s.shndx < SHN_LORESERVE && loaded(st_[o].secs[s.shndx]);
    }

    void find_existing() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (!named(s) || !in_loaded_section(o, s)) continue;
                const SymbolKey key = key_of(ob, s);
                const IndexEntry* e = index_.find(key);
                if (e && e->ambiguous) {
                    if (st_[o].secs[s.shndx].kind != SecKind::Text)
                        throw LinkError("cannot tell which " + key.display() +
                                        " to keep: several source files with that name define it");
                    out_.log.push_back("new   " + key.display() + " (ambiguous name, not redirected)");
                    e = nullptr;
                }
                st_[o].existing[k] = e;
            }
        }
    }

    // A data section either stays where the running program has it (all its named
    // symbols exist) or is loaded new. Patch compiles use -fdata-sections, so a
    // section normally holds one variable.
    void bind_data() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            std::map<uint32_t, std::vector<size_t>> by_section;
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (named(s) && in_loaded_section(o, s)) by_section[s.shndx].push_back(k);
            }
            for (auto& [sec, syms] : by_section) {
                SecState& ss = st_[o].secs[sec];
                if (ss.kind == SecKind::Text) continue;
                if (ss.kind == SecKind::ReadOnly &&
                    !std::all_of(syms.begin(), syms.end(),
                                 [&](size_t k) { return is_vtable_or_rtti(ob.symbols[k].name); }))
                    continue; // constants: the new values take effect
                size_t existing = 0;
                for (size_t k : syms) existing += st_[o].existing[k] != nullptr;
                if (existing == 0) continue;
                if (existing != syms.size())
                    throw LinkError("section " + ob.sections[sec].name + " in " + file_of(ob) +
                                    " mixes existing and new variables");
                std::optional<uint64_t> base;
                for (size_t k : syms) {
                    const ObjSymbol& s = ob.symbols[k];
                    const IndexEntry* e = st_[o].existing[k];
                    const SymbolKey key = key_of(ob, s);
                    if (e->kind == SymbolKind::Function)
                        throw RestartRequired(key.display() + " changed from a function to a variable");
                    if (e->kind == SymbolKind::Tls)
                        throw RestartRequired(key.display() + " is thread_local, not supported yet (M3)");
                    if (e->size && s.size && e->size != s.size)
                        throw RestartRequired("size of " + key.display() + " changed from " +
                                              std::to_string(e->size) + " to " + std::to_string(s.size) +
                                              " bytes");
                    const uint64_t b = e->address - s.value;
                    if (base && *base != b)
                        throw LinkError("variables in " + ob.sections[sec].name +
                                        " are not laid out as in the running program");
                    base = b;
                    out_.log.push_back("keep  " + key.display() + " at " + hex(e->address));
                }
                ss.bound_old = true;
                ss.old_base = *base;
                out_.data_kept += syms.size();
            }
        }
    }

    void collect_globals() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (s.bind == STB_LOCAL || !named(s) || !s.defined()) continue;
                if (s.shndx != SHN_ABS && !in_loaded_section(o, s)) continue;
                auto [it, inserted] = globals_.emplace(s.name, std::make_pair(o, k));
                if (!inserted && obj(it->second.first).symbols[it->second.second].bind == STB_WEAK &&
                    s.bind != STB_WEAK)
                    it->second = {o, k};
            }
        }
    }

    // Names only another module can provide, looked up in one round trip.
    void resolve_externals() {
        std::set<std::string> names;
        for_each_reloc([&](size_t o, uint32_t, const Rela& r) {
            const ObjSymbol& s = obj(o).symbols[r.sym()];
            if (s.type == STT_SECTION || s.bind == STB_LOCAL) return;
            if (in_loaded_section(o, s) || s.shndx == SHN_ABS) return;
            if (globals_.count(s.name) || index_.find({false, "", s.name}) ||
                index_.find({true, "", s.name}))
                return;
            names.insert(s.name);
        });
        if (names.empty()) return;
        const std::vector<std::string> list(names.begin(), names.end());
        const std::vector<uint64_t> addrs = resolve_(list);
        for (size_t i = 0; i < list.size() && i < addrs.size(); ++i) external_[list[i]] = addrs[i];
    }

    template <class F>
    void for_each_reloc(F&& f) const {
        for (size_t o = 0; o < st_.size(); ++o)
            for (size_t i = 0; i < st_[o].secs.size(); ++i) {
                const SecState& ss = st_[o].secs[i];
                if (!loaded(ss) || ss.bound_old) continue;
                for (const Rela& r : obj(o).sections[i].relocs) f(o, uint32_t(i), r);
            }
    }

    Target resolve_symbol(size_t o, uint32_t k, bool call, int depth = 0) const {
        const ElfObject& ob = obj(o);
        const ObjSymbol& s = ob.symbols[k];
        if (s.type == STT_SECTION) {
            if (s.shndx >= st_[o].secs.size()) throw LinkError("bad section symbol in " + ob.path);
            const SecState& ss = st_[o].secs[s.shndx];
            if (!loaded(ss))
                throw LinkError("reference to unloaded section " + ob.sections[s.shndx].name + " in " + file_of(ob));
            if (ss.bound_old) return {Target::Absolute, ss.old_base};
            return {Target::InPatch, 0, o, s.shndx};
        }
        if (s.shndx == SHN_ABS) return {Target::Absolute, s.value};
        if (in_loaded_section(o, s)) {
            const SecState& ss = st_[o].secs[s.shndx];
            if (ss.bound_old) return {Target::Absolute, ss.old_base + s.value};
            // Taking a function's address yields its canonical (original) address, so
            // pointers compare equal across old and new code; calls go straight to the new code.
            if (ss.kind == SecKind::Text && !call) {
                const IndexEntry* e = st_[o].existing[k];
                if (e && e->kind == SymbolKind::Function) return {Target::Absolute, e->address};
            }
            return {Target::InPatch, s.value, o, s.shndx};
        }
        if (s.bind == STB_LOCAL) throw LinkError("unresolvable local symbol " + s.name + " in " + file_of(ob));

        auto g = globals_.find(s.name);
        if (g != globals_.end() && depth == 0) return resolve_symbol(g->second.first, uint32_t(g->second.second), call, 1);
        if (const IndexEntry* e = index_.find({false, "", s.name})) {
            if (e->kind == SymbolKind::Function) return {Target::Absolute, call ? e->current() : e->address};
            if (e->kind == SymbolKind::Tls) throw RestartRequired("thread_local " + s.name + " is not supported yet (M3)");
            return {Target::Absolute, e->address};
        }
        if (const IndexEntry* e = index_.find({true, "", s.name})) return {Target::Absolute, e->address};
        auto x = external_.find(s.name);
        if (x != external_.end() && x->second) return {Target::External, x->second};
        if (s.bind == STB_WEAK) return {Target::Absolute, 0};
        throw LinkError("undefined symbol " + s.name + " (referenced from " + file_of(ob) + ")");
    }

    uint64_t address(const Target& t) const {
        if (t.kind == Target::InPatch) return st_[t.obj].secs[t.sec].addr + t.value;
        return t.value;
    }

    static bool is_got(uint32_t type) {
        return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX;
    }

    static std::string got_key(const Target& t) {
        if (t.kind == Target::InPatch)
            return "p" + std::to_string(t.obj) + ":" + std::to_string(t.sec) + ":" + std::to_string(t.value);
        return "a" + std::to_string(t.value);
    }

    void plan_got_and_stubs() {
        for_each_reloc([&](size_t o, uint32_t, const Rela& r) {
            const uint32_t type = r.type();
            if (is_got(type)) {
                const Target t = resolve_symbol(o, r.sym(), false);
                if (!got_index_.count(got_key(t))) {
                    got_index_[got_key(t)] = got_targets_.size();
                    got_targets_.push_back(t);
                }
            } else if (type == R_X86_64_PLT32) {
                const Target t = resolve_symbol(o, r.sym(), true);
                if (t.kind == Target::External && !stub_index_.count(t.value)) {
                    stub_index_[t.value] = stub_targets_.size();
                    stub_targets_.push_back(t.value);
                }
            }
        });
    }

    void layout() {
        const uint64_t page = opt_.page_size;
        const uint64_t start = align_up(opt_.region_cursor, page);
        auto place = [&](SecKind kind, uint64_t base, uint64_t& offset) {
            for (ObjState& os : st_)
                for (size_t i = 0; i < os.secs.size(); ++i) {
                    SecState& ss = os.secs[i];
                    if (ss.kind != kind || !loaded(ss) || ss.bound_old) continue;
                    const ObjSection& sec = os.obj->sections[i];
                    offset = align_up(offset, std::max<uint64_t>(sec.align, 1));
                    ss.addr = base + offset;
                    offset += sec.size;
                }
        };

        uint64_t off = 0;
        text_start_ = start;
        place(SecKind::Text, text_start_, off);
        off = align_up(off, 16);
        stub_base_ = text_start_ + off;
        off += 16 * stub_targets_.size();
        text_size_ = align_up(off, page);

        off = 0;
        ro_start_ = text_start_ + text_size_;
        place(SecKind::ReadOnly, ro_start_, off);
        off = align_up(off, 8);
        got_base_ = ro_start_ + off;
        off += 8 * got_targets_.size();
        ro_size_ = align_up(off, page);

        off = 0;
        data_start_ = ro_start_ + ro_size_;
        place(SecKind::Data, data_start_, off);
        data_bytes_ = off;
        place(SecKind::Bss, data_start_, off);
        data_size_ = align_up(off, page);

        const uint64_t end = data_start_ + data_size_;
        if (end > opt_.region_end || end < start)
            throw LinkError("the patch region is full; restart the program to free it");
        text_.assign(text_size_, 0xCC); // int3 between functions
        ro_.assign(ro_size_, 0);
        data_.assign(data_bytes_, 0);

        for (ObjState& os : st_)
            for (size_t i = 0; i < os.secs.size(); ++i) {
                const SecState& ss = os.secs[i];
                const ObjSection& sec = os.obj->sections[i];
                if (!loaded(ss) || ss.bound_old || !sec.data || ss.kind == SecKind::Bss) continue;
                std::memcpy(buffer_at(ss.addr, sec.size), sec.data, sec.size);
            }
    }

    uint8_t* buffer_at(uint64_t addr, uint64_t size) {
        auto in = [&](uint64_t base, std::vector<uint8_t>& buf) -> uint8_t* {
            if (addr >= base && size <= buf.size() && addr - base <= buf.size() - size) return buf.data() + (addr - base);
            return nullptr;
        };
        if (uint8_t* p = in(text_start_, text_)) return p;
        if (uint8_t* p = in(ro_start_, ro_)) return p;
        if (uint8_t* p = in(data_start_, data_)) return p;
        throw LinkError("internal error: address " + hex(addr) + " outside the patch image");
    }

    void apply_relocations() {
        for (size_t i = 0; i < stub_targets_.size(); ++i) {
            uint8_t* p = buffer_at(stub_base_ + 16 * i, 14);
            static const uint8_t jmp_rip[6] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp *0(%rip)
            std::memcpy(p, jmp_rip, 6);
            put64(p + 6, stub_targets_[i]);
        }

        for_each_reloc([&](size_t o, uint32_t si, const Rela& r) {
            const ElfObject& ob = obj(o);
            const ObjSection& sec = ob.sections[si];
            const SecState& ss = st_[o].secs[si];
            const uint32_t type = r.type();
            if (type == R_X86_64_NONE) return;
            const uint64_t width = (type == R_X86_64_64 || type == R_X86_64_PC64 || type == R_X86_64_SIZE64) ? 8 : 4;
            if (ss.kind == SecKind::Bss || r.offset > sec.size || width > sec.size - r.offset)
                throw LinkError("relocation outside section " + sec.name + " in " + file_of(ob));
            uint8_t* p = buffer_at(ss.addr + r.offset, width);
            const uint64_t P = ss.addr + r.offset;
            const int64_t A = r.addend;
            const ObjSymbol& sym = ob.symbols[r.sym()];

            auto fail_range = [&](uint64_t target) {
                const std::string what = sym.type == STT_SECTION ? ob.sections[sym.shndx].name : sym.name;
                if (type == R_X86_64_PC32 && external_.count(sym.name))
                    throw RestartRequired(file_of(ob) + " now uses " + sym.name +
                                          " from a shared library directly; the executable has no copy of it");
                throw LinkError("relocation to " + what + " at " + hex(target) + " is out of range in " + file_of(ob));
            };
            auto pc32 = [&](uint64_t target) {
                const int64_t v = int64_t(target + uint64_t(A) - P);
                if (v != int64_t(int32_t(v))) fail_range(target);
                put32(p, uint32_t(int32_t(v)));
            };

            switch (type) {
            case R_X86_64_64: put64(p, address(resolve_symbol(o, r.sym(), false)) + uint64_t(A)); break;
            case R_X86_64_PC64: put64(p, address(resolve_symbol(o, r.sym(), false)) + uint64_t(A) - P); break;
            case R_X86_64_PC32: pc32(address(resolve_symbol(o, r.sym(), false))); break;
            case R_X86_64_PLT32: {
                const Target t = resolve_symbol(o, r.sym(), true);
                pc32(t.kind == Target::External ? stub_base_ + 16 * stub_index_.at(t.value) : address(t));
                break;
            }
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX: {
                const Target t = resolve_symbol(o, r.sym(), false);
                pc32(got_base_ + 8 * got_index_.at(got_key(t)));
                break;
            }
            case R_X86_64_32: case R_X86_64_32S: {
                const uint64_t v = address(resolve_symbol(o, r.sym(), false)) + uint64_t(A);
                const bool fits = type == R_X86_64_32 ? v <= 0xffffffffull : int64_t(v) == int64_t(int32_t(v));
                if (!fits)
                    throw LinkError(file_of(ob) + " uses 32-bit absolute addresses; build with -fPIE (the default)");
                put32(p, uint32_t(v));
                break;
            }
            case R_X86_64_SIZE32: put32(p, uint32_t(sym.size + uint64_t(A))); break;
            case R_X86_64_SIZE64: put64(p, sym.size + uint64_t(A)); break;
            case R_X86_64_TLSGD: case R_X86_64_TLSLD: case R_X86_64_DTPOFF32: case R_X86_64_GOTTPOFF:
            case R_X86_64_TPOFF32: case R_X86_64_DTPMOD64: case R_X86_64_DTPOFF64: case R_X86_64_TPOFF64:
            case R_X86_64_GOTPC32_TLSDESC: case R_X86_64_TLSDESC_CALL:
                throw RestartRequired(file_of(ob) + " accesses thread_local variables, not supported yet (M3)");
            default:
                throw LinkError("unsupported relocation type " + std::to_string(type) + " in " + sec.name +
                                " of " + file_of(ob));
            }
        });

        for (size_t i = 0; i < got_targets_.size(); ++i) put64(buffer_at(got_base_ + 8 * i, 8), address(got_targets_[i]));
    }

    void make_sites_and_delta() {
        std::set<uint64_t> redirected; // entries already handled (aliases share an address)
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (!named(s) || !in_loaded_section(o, s)) continue;
                const SecState& ss = st_[o].secs[s.shndx];
                if (ss.bound_old) continue;
                const SymbolKey key = key_of(ob, s);
                const IndexEntry* e = st_[o].existing[k];
                const uint64_t addr = ss.addr + s.value;

                if (ss.kind != SecKind::Text) {
                    if (!e && s.type != STT_NOTYPE) {
                        IndexEntry ne;
                        ne.kind = SymbolKind::Data;
                        ne.address = addr;
                        ne.size = s.size;
                        ne.writable = ss.kind == SecKind::Data || ss.kind == SecKind::Bss;
                        out_.delta.symbols.push_back({key, ne});
                        ++out_.data_new;
                        out_.log.push_back("new   " + key.display() + " at " + hex(addr));
                    }
                    continue;
                }
                if (s.type != STT_FUNC) continue;

                const ObjSection& sec = ob.sections[s.shndx];
                FunctionVersion v;
                v.entry = addr;
                if (s.value >= kPadBytes && sec.data) {
                    const uint64_t from = s.value - kPadBytes;
                    v.site = analyze_patch_site(sec.data + from, size_t(std::min<uint64_t>(kSiteProbeBytes, sec.size - from)));
                }

                if (e && e->kind != SymbolKind::Function)
                    throw RestartRequired(key.display() + " changed from a variable to a function");
                if (!e) {
                    IndexEntry ne;
                    ne.kind = SymbolKind::Function;
                    ne.address = addr;
                    ne.size = s.size;
                    ne.versions.push_back(v);
                    out_.delta.symbols.push_back({key, ne});
                    ++out_.functions_new;
                    out_.log.push_back("new   " + key.display() + " at " + hex(addr));
                    continue;
                }
                size_t sites = 0;
                for (const FunctionVersion& old : e->versions) {
                    if (!old.site) continue;
                    ++sites;
                    if (!redirected.insert(old.entry).second) continue;
                    proto::PatchSite site;
                    site.pad_addr = old.entry - kPadBytes;
                    site.entry_addr = old.entry + old.site->entry_offset;
                    site.target = addr;
                    site.entry_bytes[0] = old.site->short_jump[0];
                    site.entry_bytes[1] = old.site->short_jump[1];
                    out_.patch.sites.push_back(site);
                }
                if (sites) {
                    ++out_.functions_redirected;
                    out_.log.push_back("patch " + key.display() + " " + hex(e->address) + " -> " + hex(addr));
                } else {
                    out_.log.push_back("new   " + key.display() + " (original has no patch padding; old callers keep it)");
                }
                out_.delta.versions.push_back({key, v});
            }
        }
    }

    void emit_segments() {
        if (text_size_) out_.patch.segments.push_back({text_start_, text_size_, proto::kProtRead | proto::kProtExec, text_});
        if (ro_size_) out_.patch.segments.push_back({ro_start_, ro_size_, proto::kProtRead, ro_});
        if (data_size_) out_.patch.segments.push_back({data_start_, data_size_, proto::kProtRead | proto::kProtWrite, data_});
        out_.bytes_used = data_start_ + data_size_ - opt_.region_cursor;
    }

    const SymbolIndex& index_;
    const ExternalResolver& resolve_;
    LinkOptions opt_;
    std::vector<ObjState> st_;
    std::unordered_map<std::string, std::pair<size_t, size_t>> globals_;
    std::unordered_map<std::string, uint64_t> external_;
    std::unordered_map<std::string, size_t> got_index_;
    std::vector<Target> got_targets_;
    std::unordered_map<uint64_t, size_t> stub_index_;
    std::vector<uint64_t> stub_targets_;
    uint64_t text_start_ = 0, text_size_ = 0, stub_base_ = 0;
    uint64_t ro_start_ = 0, ro_size_ = 0, got_base_ = 0;
    uint64_t data_start_ = 0, data_size_ = 0, data_bytes_ = 0;
    std::vector<uint8_t> text_, ro_, data_;
    LinkResult out_;
};

} // namespace

LinkResult link_patch(const std::vector<const ElfObject*>& objects, const SymbolIndex& index,
                      const ExternalResolver& resolve, const LinkOptions& options) {
    return Linker(objects, index, resolve, options).run();
}

} // namespace hotpp::link
