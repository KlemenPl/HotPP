#include "host/link/linker.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
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

enum class SecKind { Skip, Text, ReadOnly, Data, Bss, EhFrame, InitArray, Tls };

struct SecState {
    SecKind kind = SecKind::Skip;
    bool discarded = false;   // duplicate COMDAT copy
    bool bound_old = false;   // stays where the running program has it
    uint64_t old_base = 0;    // bound_old: address (TLS: template offset) of section offset 0
    uint64_t addr = 0;        // loaded: address in the patch region
    bool init_code = false;   // part of a dynamic initializer
    bool needs_shadow = false; // bound_old data an initializer writes: gets a scratch copy
    uint64_t shadow_addr = 0;
};

struct ObjState {
    const ElfObject* obj = nullptr;
    std::vector<SecState> secs;
    std::vector<const IndexEntry*> existing; // per symbol: what it replaces, if anything
};

struct Target {
    enum Kind { Absolute, InPatch, External, Shadow, Tls } kind = Absolute;
    uint64_t value = 0; // Absolute/External: address. InPatch/Shadow: section offset. Tls: template offset
    size_t obj = 0;
    uint32_t sec = 0;
};

struct GotSlot {
    bool is_value = false; // a constant (TLS offsets, module IDs) instead of a target's address
    Target target;
    uint64_t value = 0;
};

struct InitEntry {
    int priority = 0;
    size_t order = 0;
    Target target;
    std::set<std::pair<size_t, uint32_t>> closure; // text sections of this initializer
};

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool is_vtable_or_rtti(const std::string& n) {
    return starts_with(n, "_ZTV") || starts_with(n, "_ZTI") || starts_with(n, "_ZTS") || starts_with(n, "_ZTT");
}

// Functions the compiler generates to run (or undo) dynamic initialization.
bool is_initializer_function(const std::string& n) {
    return starts_with(n, "_GLOBAL__sub_I_") || starts_with(n, "_GLOBAL__I_") ||
           starts_with(n, "__cxx_global_var_init") || starts_with(n, "__cxx_global_array_dtor") ||
           n.find("__static_initialization_and_destruction") != std::string::npos || starts_with(n, "__tcf_") ||
           starts_with(n, "__dtor_");
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

std::string file_of(const ElfObject& o) { return o.source_name.empty() ? o.path : o.source_name; }

int init_priority(const std::string& name) {
    // .init_array.NNNNN runs in ascending priority, before plain .init_array.
    if (starts_with(name, ".init_array.")) return std::atoi(name.c_str() + 12);
    return 65536;
}

SecKind classify(const ObjSection& s, const ElfObject& o, std::vector<std::string>& notes) {
    if (!(s.flags & SHF_ALLOC)) return SecKind::Skip;
    switch (s.type) {
    case SHT_GROUP: case SHT_NOTE: case SHT_RELA: case SHT_SYMTAB: case SHT_STRTAB: return SecKind::Skip;
    default: break;
    }
    if (s.type == SHT_X86_64_UNWIND || s.name == ".eh_frame") return SecKind::EhFrame;
    if (s.type == SHT_INIT_ARRAY || starts_with(s.name, ".init_array")) return SecKind::InitArray;
    if (s.type == SHT_FINI_ARRAY || s.type == SHT_PREINIT_ARRAY || starts_with(s.name, ".fini_array") ||
        starts_with(s.name, ".ctors") || starts_with(s.name, ".dtors")) {
        if (s.size && (s.type == SHT_FINI_ARRAY || starts_with(s.name, ".fini_array")))
            notes.push_back(file_of(o) + ": __attribute__((destructor)) functions of the patch are not registered");
        return SecKind::Skip;
    }
    if (s.name == "__patchable_function_entries") return SecKind::Skip;
    if (s.flags & SHF_TLS) return SecKind::Tls;
    if (s.flags & SHF_EXECINSTR) return SecKind::Text;
    if (s.type == SHT_NOBITS) return SecKind::Bss;
    if (!(s.flags & SHF_WRITE) || starts_with(s.name, ".data.rel.ro")) return SecKind::ReadOnly;
    return SecKind::Data;
}

bool is_data(SecKind k) { return k == SecKind::Data || k == SecKind::Bss; }

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

bool is_tls_reloc(uint32_t t) {
    switch (t) {
    case R_X86_64_TLSGD: case R_X86_64_TLSLD: case R_X86_64_DTPOFF32: case R_X86_64_GOTTPOFF:
    case R_X86_64_TPOFF32: case R_X86_64_DTPMOD64: case R_X86_64_DTPOFF64: case R_X86_64_TPOFF64:
    case R_X86_64_GOTPC32_TLSDESC: case R_X86_64_TLSDESC_CALL:
        return true;
    default: return false;
    }
}

class Linker {
public:
    Linker(const std::vector<const ElfObject*>& objects, const SymbolIndex& index, const ExternalResolver& resolve,
           const LinkOptions& opt)
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
        find_initializers();
        collect_canonical_entries();
        plan_got_and_stubs();
        layout();
        apply_relocations();
        make_sites_and_delta();
        emit();
        if (opt_.debug_objects) build_debug_objects();
        return std::move(out_);
    }

private:
    const ElfObject& obj(size_t o) const { return *st_[o].obj; }

    // Copied into the patch image.
    static bool loaded(const SecState& s) {
        if (s.discarded || s.bound_old) return false;
        return s.kind == SecKind::Text || s.kind == SecKind::ReadOnly || is_data(s.kind) || s.kind == SecKind::EhFrame;
    }

    // Defines symbols the patch can refer to (loaded, or bound to the old program).
    bool defined_here(size_t o, const ObjSymbol& s) const {
        if (!s.defined() || s.shndx >= SHN_LORESERVE) return false;
        const SecState& ss = st_[o].secs[s.shndx];
        return !ss.discarded && ss.kind != SecKind::Skip && ss.kind != SecKind::InitArray && ss.kind != SecKind::EhFrame;
    }

    void dedupe_comdats() {
        std::unordered_map<std::string, std::pair<size_t, size_t>> kept; // signature -> (object, group)
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t g = 0; g < ob.groups.size(); ++g) {
                const object::ObjGroup& grp = ob.groups[g];
                if (!(grp.flags & GRP_COMDAT)) continue;
                auto [it, inserted] = kept.emplace(grp.signature, std::make_pair(o, g));
                if (inserted) continue;
                // A duplicate copy: references to its sections go to the kept copy's
                // section of the same name (unwind tables, exception tables).
                const auto [ko, kg] = it->second;
                const ElfObject& kob = obj(ko);
                for (uint32_t m : grp.members) {
                    st_[o].secs[m].discarded = true;
                    for (uint32_t km : kob.groups[kg].members)
                        if (kob.sections[km].name == ob.sections[m].name) kept_copy_[{o, m}] = {ko, km};
                }
            }
        }
    }

    void classify_sections() {
        for (ObjState& os : st_)
            for (size_t i = 0; i < os.secs.size(); ++i)
                if (!os.secs[i].discarded) os.secs[i].kind = classify(os.obj->sections[i], *os.obj, out_.notes);
    }

    void find_existing() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (!named(s) || !defined_here(o, s)) continue;
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
                if (named(s) && defined_here(o, s)) by_section[s.shndx].push_back(k);
            }
            for (size_t sec = 0; sec < st_[o].secs.size(); ++sec) {
                const SecState& ss = st_[o].secs[sec];
                if (ss.kind == SecKind::Tls && !ss.discarded && ob.sections[sec].size && !by_section.count(uint32_t(sec)))
                    throw RestartRequired(file_of(ob) + " adds thread-local storage (" + ob.sections[sec].name + ")");
            }
            for (auto& [sec, syms] : by_section) {
                SecState& ss = st_[o].secs[sec];
                if (ss.kind == SecKind::Text) continue;
                const bool tls = ss.kind == SecKind::Tls;
                if (ss.kind == SecKind::ReadOnly &&
                    !std::all_of(syms.begin(), syms.end(), [&](size_t k) { return is_vtable_or_rtti(ob.symbols[k].name); }))
                    continue; // constants: the new values take effect
                size_t existing = 0;
                for (size_t k : syms) existing += st_[o].existing[k] != nullptr;
                if (existing == 0 && !tls) continue;
                if (existing != syms.size()) {
                    for (size_t k : syms)
                        if (!st_[o].existing[k] && tls)
                            throw RestartRequired("new thread_local " + key_of(ob, ob.symbols[k]).display());
                    throw LinkError("section " + ob.sections[sec].name + " in " + file_of(ob) +
                                    " mixes existing and new variables");
                }
                std::optional<uint64_t> base;
                for (size_t k : syms) {
                    const ObjSymbol& s = ob.symbols[k];
                    const IndexEntry* e = st_[o].existing[k];
                    const SymbolKey key = key_of(ob, s);
                    if (e->kind == SymbolKind::Function)
                        throw RestartRequired(key.display() + " changed from a function to a variable");
                    if (tls != (e->kind == SymbolKind::Tls))
                        throw RestartRequired(key.display() + (tls ? " became thread_local" : " is no longer thread_local"));
                    if (e->size && s.size && e->size != s.size)
                        throw RestartRequired("size of " + key.display() + " changed from " + std::to_string(e->size) +
                                              " to " + std::to_string(s.size) + " bytes");
                    const uint64_t b = e->address - s.value;
                    if (base && *base != b)
                        throw LinkError("variables in " + ob.sections[sec].name + " are not laid out as in the running program");
                    base = b;
                    out_.log.push_back("keep  " + key.display() + (tls ? " at TLS offset " : " at ") + hex(e->address));
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
                if (s.shndx != SHN_ABS && !defined_here(o, s)) continue;
                auto [it, inserted] = globals_.emplace(s.name, std::make_pair(o, k));
                if (!inserted && obj(it->second.first).symbols[it->second.second].bind == STB_WEAK && s.bind != STB_WEAK)
                    it->second = {o, k};
            }
        }
    }

    template <class F>
    void for_each_reloc(F&& f) const {
        for (size_t o = 0; o < st_.size(); ++o)
            for (size_t i = 0; i < st_[o].secs.size(); ++i) {
                if (!loaded(st_[o].secs[i])) continue;
                for (const Rela& r : obj(o).sections[i].relocs) f(o, uint32_t(i), r);
            }
    }

    // Names only another module can provide, looked up in one round trip.
    void resolve_externals() {
        std::set<std::string> names;
        auto consider = [&](size_t o, const Rela& r) {
            const ObjSymbol& s = obj(o).symbols[r.sym()];
            if (s.type == STT_SECTION || s.bind == STB_LOCAL) return;
            if (defined_here(o, s) || s.shndx == SHN_ABS) return;
            if (globals_.count(s.name) || index_.find({false, "", s.name}) || index_.find({true, "", s.name})) return;
            names.insert(s.name);
        };
        for_each_reloc([&](size_t o, uint32_t, const Rela& r) { consider(o, r); });
        for (size_t o = 0; o < st_.size(); ++o)
            for (size_t i = 0; i < st_[o].secs.size(); ++i)
                if (st_[o].secs[i].kind == SecKind::InitArray && !st_[o].secs[i].discarded)
                    for (const Rela& r : obj(o).sections[i].relocs) consider(o, r);
        if (names.empty()) return;
        const std::vector<std::string> list(names.begin(), names.end());
        const std::vector<uint64_t> addrs = resolve_(list);
        for (size_t i = 0; i < list.size() && i < addrs.size(); ++i) external_[list[i]] = addrs[i];
    }

    Target section_target(size_t o, uint32_t sec, uint64_t offset, bool in_init) const {
        const SecState& ss = st_[o].secs[sec];
        if (ss.discarded) {
            auto k = kept_copy_.find({o, sec});
            if (k == kept_copy_.end())
                throw LinkError("reference to discarded section " + obj(o).sections[sec].name + " in " + file_of(obj(o)));
            return section_target(k->second.first, k->second.second, offset, in_init);
        }
        if (ss.kind == SecKind::Tls) {
            if (!ss.bound_old) throw RestartRequired(file_of(obj(o)) + " adds thread-local storage");
            return {Target::Tls, ss.old_base + offset};
        }
        if (ss.bound_old) {
            if (in_init && ss.needs_shadow) return {Target::Shadow, offset, o, sec};
            return {Target::Absolute, ss.old_base + offset};
        }
        if (!loaded(ss))
            throw LinkError("reference to unloaded section " + obj(o).sections[sec].name + " in " + file_of(obj(o)));
        return {Target::InPatch, offset, o, sec};
    }

    // What a relocation against symbol k refers to. `call`: it's a call (calls go to
    // new code, address-of goes to the canonical address). `in_init`: the reference
    // is inside a dynamic initializer, which writes scratch copies of existing data.
    Target resolve_symbol(size_t o, uint32_t k, bool call, bool in_init, int depth = 0) const {
        const ElfObject& ob = obj(o);
        const ObjSymbol& s = ob.symbols[k];
        if (s.type == STT_SECTION) {
            if (s.shndx >= st_[o].secs.size()) throw LinkError("bad section symbol in " + ob.path);
            return section_target(o, s.shndx, 0, in_init);
        }
        if (s.shndx == SHN_ABS) return {Target::Absolute, s.value};
        if (defined_here(o, s)) {
            const SecState& ss = st_[o].secs[s.shndx];
            // Taking a function's address yields its canonical (original) address, so
            // pointers compare equal across old and new code; calls go straight to the new code.
            if (ss.kind == SecKind::Text && !call) {
                const IndexEntry* e = st_[o].existing[k];
                if (e && e->kind == SymbolKind::Function) return {Target::Absolute, e->address};
            }
            return section_target(o, s.shndx, s.value, in_init);
        }
        if (s.bind == STB_LOCAL) {
            if (s.defined() && s.shndx < SHN_LORESERVE && st_[o].secs[s.shndx].discarded)
                return section_target(o, s.shndx, s.value, in_init); // via the kept COMDAT copy
            throw LinkError("unresolvable local symbol " + s.name + " in " + file_of(ob));
        }

        auto g = globals_.find(s.name);
        if (g != globals_.end() && depth == 0)
            return resolve_symbol(g->second.first, uint32_t(g->second.second), call, in_init, 1);
        if (const IndexEntry* e = index_.find({false, "", s.name})) {
            if (e->kind == SymbolKind::Function) return {Target::Absolute, call ? e->current() : e->address};
            if (e->kind == SymbolKind::Tls) return {Target::Tls, e->address};
            return {Target::Absolute, e->address};
        }
        if (const IndexEntry* e = index_.find({true, "", s.name})) return {Target::Absolute, e->address};
        auto x = external_.find(s.name);
        if (x != external_.end() && x->second) return {Target::External, x->second};
        if (s.bind == STB_WEAK) return {Target::Absolute, 0};
        throw LinkError("undefined symbol " + s.name + " (referenced from " + file_of(ob) + ")");
    }

    uint64_t address(const Target& t) const {
        switch (t.kind) {
        case Target::InPatch: return st_[t.obj].secs[t.sec].addr + t.value;
        case Target::Shadow: return st_[t.obj].secs[t.sec].shadow_addr + t.value;
        case Target::Tls: throw LinkError("a thread_local variable is accessed like an ordinary one");
        default: return t.value;
        }
    }

    // Text sections in this object holding a compiler-generated initializer function.
    bool initializer_section(size_t o, uint32_t sec) const {
        const ElfObject& ob = obj(o);
        for (const ObjSymbol& s : ob.symbols)
            if (s.shndx == sec && s.type == STT_FUNC && is_initializer_function(s.name)) return true;
        return false;
    }

    // Dynamic initializers: .init_array entries and the compiler-generated helpers
    // they call. An initializer runs only if it sets up a *new* variable; inside it,
    // existing variables are replaced by scratch copies, so their state isn't reset.
    void find_initializers() {
        std::vector<InitEntry> entries;
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t i = 0; i < st_[o].secs.size(); ++i) {
                const SecState& ss = st_[o].secs[i];
                if (ss.kind != SecKind::InitArray || ss.discarded) continue;
                std::vector<Rela> relocs = ob.sections[i].relocs;
                std::sort(relocs.begin(), relocs.end(), [](const Rela& a, const Rela& b) { return a.offset < b.offset; });
                for (const Rela& r : relocs) {
                    if (r.type() != R_X86_64_64) throw LinkError("unexpected relocation in " + ob.sections[i].name);
                    Target t = resolve_symbol(o, r.sym(), true, false);
                    if (t.kind != Target::InPatch || st_[t.obj].secs[t.sec].kind != SecKind::Text)
                        throw LinkError("initializer in " + file_of(ob) + " is not in the patch's code");
                    t.value += uint64_t(r.addend);
                    InitEntry e;
                    e.priority = init_priority(ob.sections[i].name);
                    e.order = entries.size();
                    e.target = t;
                    entries.push_back(std::move(e));
                }
            }
        }
        if (entries.empty()) return;
        std::stable_sort(entries.begin(), entries.end(),
                         [](const InitEntry& a, const InitEntry& b) { return a.priority < b.priority; });

        for (InitEntry& e : entries) {
            std::vector<std::pair<size_t, uint32_t>> work{{e.target.obj, e.target.sec}};
            e.closure.insert(work.back());
            while (!work.empty()) {
                const auto [o, sec] = work.back();
                work.pop_back();
                for (const Rela& r : obj(o).sections[sec].relocs) {
                    const ObjSymbol& s = obj(o).symbols[r.sym()];
                    if (!s.defined() || s.shndx >= st_[o].secs.size()) continue;
                    if (s.type != STT_SECTION && !(s.type == STT_FUNC && s.bind == STB_LOCAL)) continue;
                    const SecState& ts = st_[o].secs[s.shndx];
                    if (ts.kind != SecKind::Text || ts.discarded || !initializer_section(o, s.shndx)) continue;
                    if (e.closure.insert({o, s.shndx}).second) work.push_back({o, s.shndx});
                }
            }
        }

        size_t rerun_existing = 0;
        for (InitEntry& e : entries) {
            bool new_data = false;
            std::set<std::pair<size_t, uint32_t>> old_data;
            for (const auto& [o, sec] : e.closure)
                for (const Rela& r : obj(o).sections[sec].relocs) {
                    if (is_tls_reloc(r.type())) continue;
                    const ObjSymbol& s = obj(o).symbols[r.sym()];
                    if (!(s.type == STT_SECTION || defined_here(o, s))) continue;
                    const uint32_t ts = s.shndx;
                    if (ts >= st_[o].secs.size()) continue;
                    const SecState& tss = st_[o].secs[ts];
                    if (!is_data(tss.kind) || tss.discarded) continue;
                    if (tss.bound_old) old_data.insert({o, ts});
                    else new_data = true;
                }
            if (!new_data) {
                out_.log.push_back("skip  initializer in " + file_of(obj(e.target.obj)) + " (its variables all exist)");
                continue;
            }
            for (const auto& [o, sec] : e.closure) st_[o].secs[sec].init_code = true;
            for (const auto& [o, sec] : old_data) st_[o].secs[sec].needs_shadow = true;
            rerun_existing += old_data.size();
            run_.push_back(e.target);
            out_.log.push_back("run   initializer in " + file_of(obj(e.target.obj)));
        }
        out_.initializers_run = run_.size();
        if (rerun_existing)
            out_.notes.push_back("the initializers that set up new variables also re-ran for " +
                                 std::to_string(rerun_existing) +
                                 " existing ones, into scratch copies; side effects such as registration or I/O happened again");
    }

    // GCC takes the address of a local function through its *section* symbol
    // (`lea .text._ZL3foov+1(%rip)`), which resolve_symbol() can't tell from any
    // other reference into the section. When the referenced spot is exactly the
    // entry of a function that already exists, use its canonical address, as for
    // symbol references. Unwind tables must describe the new code, so they're exempt.
    // A PC32 relocation is a call or jump when the rel32 follows a call/jmp/jcc opcode.
    // (A RIP-relative operand's ModRM byte can never be E8/E9.)
    bool is_branch(size_t o, uint32_t sec, const Rela& r) const {
        const uint32_t type = r.type();
        if (type == R_X86_64_PLT32) return true;
        if (type != R_X86_64_PC32 || st_[o].secs[sec].kind != SecKind::Text) return false;
        const ObjSection& s = obj(o).sections[sec];
        if (!s.data || r.offset < 1 || r.offset > s.size) return false;
        const uint8_t op = s.data[r.offset - 1];
        if (op == 0xE8 || op == 0xE9) return true;
        return r.offset >= 2 && s.data[r.offset - 2] == 0x0F && (op & 0xF0) == 0x80;
    }

    Target canonical_section_target(size_t o, uint32_t from_sec, const Rela& r, const Target& t) const {
        const uint32_t type = r.type();
        if (t.kind != Target::InPatch || is_branch(o, from_sec, r)) return t;
        if (st_[o].secs[from_sec].kind == SecKind::EhFrame) return t;
        const ObjSymbol& s = obj(o).symbols[r.sym()];
        if (s.type != STT_SECTION || st_[t.obj].secs[t.sec].kind != SecKind::Text) return t;
        // The byte the reference means: RIP-relative operands end the instruction, so
        // a PC32 addend is the offset minus 4.
        const int64_t entry = type == R_X86_64_PC32 ? r.addend + 4 : r.addend;
        auto it = canonical_.find({t.obj, t.sec});
        if (it == canonical_.end()) return t;
        for (const auto& [offset, address] : it->second)
            if (int64_t(offset) == entry) return {Target::Absolute, address - offset};
        return t;
    }

    void collect_canonical_entries() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const IndexEntry* e = st_[o].existing[k];
                const ObjSymbol& s = ob.symbols[k];
                if (e && e->kind == SymbolKind::Function && s.type == STT_FUNC &&
                    st_[o].secs[s.shndx].kind == SecKind::Text)
                    canonical_[{o, s.shndx}].emplace_back(s.value, e->address);
            }
        }
    }

    static bool is_got(uint32_t type) {
        return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX;
    }

    static std::string target_key(const Target& t) {
        if (t.kind == Target::InPatch || t.kind == Target::Shadow)
            return std::string(t.kind == Target::InPatch ? "p" : "s") + std::to_string(t.obj) + ":" +
                   std::to_string(t.sec) + ":" + std::to_string(t.value);
        return "a" + std::to_string(t.value);
    }

    size_t got_slot(const std::string& key, std::initializer_list<GotSlot> slots) {
        auto it = got_index_.find(key);
        if (it != got_index_.end()) return it->second;
        const size_t first = got_.size();
        got_.insert(got_.end(), slots);
        got_index_[key] = first;
        return first;
    }

    uint64_t tpoff(const Target& t) const {
        if (t.kind != Target::Tls) throw LinkError("TLS access to a variable that isn't thread_local");
        if (!opt_.tls_static) throw LinkError("the module's TLS isn't at a fixed offset from the thread pointer");
        return t.value + uint64_t(opt_.tls_offset);
    }

    uint64_t tls_module() const {
        if (!opt_.tls_module_id) throw LinkError("the module has no TLS segment");
        return opt_.tls_module_id;
    }

    void plan_got_and_stubs() {
        for_each_reloc([&](size_t o, uint32_t si, const Rela& r) {
            const uint32_t type = r.type();
            const bool in_init = st_[o].secs[si].init_code;
            if (is_got(type)) {
                const Target t = canonical_section_target(o, si, r, resolve_symbol(o, r.sym(), false, in_init));
                got_slot(target_key(t), {GotSlot{false, t, 0}});
            } else if (is_branch(o, si, r)) {
                const Target t = resolve_symbol(o, r.sym(), true, in_init);
                if (t.kind == Target::External && !stub_index_.count(t.value)) {
                    stub_index_[t.value] = stub_targets_.size();
                    stub_targets_.push_back(t.value);
                }
            } else if (type == R_X86_64_GOTTPOFF) {
                const Target t = resolve_symbol(o, r.sym(), false, false);
                got_slot("t" + std::to_string(t.value), {GotSlot{true, {}, tpoff(t)}});
            } else if (type == R_X86_64_TLSGD) {
                const Target t = resolve_symbol(o, r.sym(), false, false);
                if (t.kind != Target::Tls) throw LinkError("TLS access to a variable that isn't thread_local");
                // tls_index {module ID, offset} for __tls_get_addr.
                got_slot("g" + std::to_string(t.value), {GotSlot{true, {}, tls_module()}, GotSlot{true, {}, t.value}});
            } else if (type == R_X86_64_TLSLD) {
                got_slot("l", {GotSlot{true, {}, tls_module()}, GotSlot{true, {}, 0}});
            } else if (type == R_X86_64_GOTPC32_TLSDESC || type == R_X86_64_TLSDESC_CALL) {
                throw LinkError(file_of(obj(o)) + " uses TLS descriptors (-mtls-dialect=gnu2), which are not supported");
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
                    if (ss.kind != kind || !loaded(ss)) continue;
                    const ObjSection& sec = os.obj->sections[i];
                    offset = align_up(offset, std::max<uint64_t>(sec.align, 1));
                    ss.addr = base + offset;
                    offset += sec.size;
                }
        };
        auto place_shadows = [&](SecKind kind, uint64_t base, uint64_t& offset) {
            for (ObjState& os : st_)
                for (size_t i = 0; i < os.secs.size(); ++i) {
                    SecState& ss = os.secs[i];
                    if (ss.kind != kind || !ss.needs_shadow) continue;
                    const ObjSection& sec = os.obj->sections[i];
                    offset = align_up(offset, std::max<uint64_t>(sec.align, 1));
                    ss.shadow_addr = base + offset;
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
        off += 8 * got_.size();
        // Unwind tables back to back (FDEs find their CIE by relative offset), then
        // the zero terminator __register_frame expects.
        off = align_up(off, 8);
        eh_frame_ = 0;
        for (ObjState& os : st_)
            for (size_t i = 0; i < os.secs.size(); ++i) {
                SecState& ss = os.secs[i];
                if (ss.kind != SecKind::EhFrame || !loaded(ss) || os.obj->sections[i].size == 0) continue;
                if (os.obj->sections[i].size % 4)
                    throw LinkError("malformed .eh_frame in " + file_of(*os.obj));
                if (!eh_frame_) eh_frame_ = ro_start_ + off;
                ss.addr = ro_start_ + off;
                off += os.obj->sections[i].size;
            }
        if (eh_frame_) off += 4;
        ro_size_ = align_up(off, page);

        off = 0;
        data_start_ = ro_start_ + ro_size_;
        place(SecKind::Data, data_start_, off);
        place_shadows(SecKind::Data, data_start_, off);
        data_bytes_ = off;
        place(SecKind::Bss, data_start_, off);
        place_shadows(SecKind::Bss, data_start_, off);
        data_size_ = align_up(off, page);

        const uint64_t end = data_start_ + data_size_;
        if (end > opt_.region_end || end < start) throw LinkError("the patch region is full; restart the program to free it");
        text_.assign(text_size_, 0xCC); // int3 between functions
        ro_.assign(ro_size_, 0);
        data_.assign(data_bytes_, 0);

        for (ObjState& os : st_)
            for (size_t i = 0; i < os.secs.size(); ++i) {
                const SecState& ss = os.secs[i];
                const ObjSection& sec = os.obj->sections[i];
                if (!sec.data || ss.kind == SecKind::Bss) continue;
                if (loaded(ss)) std::memcpy(buffer_at(ss.addr, sec.size), sec.data, sec.size);
                if (ss.needs_shadow) std::memcpy(buffer_at(ss.shadow_addr, sec.size), sec.data, sec.size);
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
            const bool in_init = ss.init_code;
            const uint32_t type = r.type();
            if (type == R_X86_64_NONE) return;
            const uint64_t width =
                (type == R_X86_64_64 || type == R_X86_64_PC64 || type == R_X86_64_SIZE64 || type == R_X86_64_TPOFF64 ||
                 type == R_X86_64_DTPOFF64 || type == R_X86_64_DTPMOD64)
                    ? 8
                    : 4;
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
            auto signed32 = [&](int64_t v) {
                if (v != int64_t(int32_t(v))) throw LinkError("TLS offset out of range in " + file_of(ob));
                put32(p, uint32_t(int32_t(v)));
            };
            const bool branch = is_branch(o, si, r);
            auto sym_target = [&](bool) {
                return canonical_section_target(o, si, r, resolve_symbol(o, r.sym(), branch, in_init));
            };

            switch (type) {
            case R_X86_64_64: put64(p, address(sym_target(false)) + uint64_t(A)); break;
            case R_X86_64_PC64: put64(p, address(sym_target(false)) + uint64_t(A) - P); break;
            case R_X86_64_PC32: case R_X86_64_PLT32: {
                // Calls and jumps into shared libraries go through stubs.
                const Target t = sym_target(branch);
                pc32(branch && t.kind == Target::External ? stub_base_ + 16 * stub_index_.at(t.value) : address(t));
                break;
            }
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX:
                pc32(got_base_ + 8 * got_index_.at(target_key(sym_target(false))));
                break;
            case R_X86_64_32: case R_X86_64_32S: {
                const uint64_t v = address(sym_target(false)) + uint64_t(A);
                const bool fits = type == R_X86_64_32 ? v <= 0xffffffffull : int64_t(v) == int64_t(int32_t(v));
                if (!fits) throw LinkError(file_of(ob) + " uses 32-bit absolute addresses; build with -fPIE (the default)");
                put32(p, uint32_t(v));
                break;
            }
            case R_X86_64_SIZE32: put32(p, uint32_t(sym.size + uint64_t(A))); break;
            case R_X86_64_SIZE64: put64(p, sym.size + uint64_t(A)); break;
            case R_X86_64_TPOFF32: signed32(int64_t(tpoff(sym_target(false))) + A); break;
            case R_X86_64_TPOFF64: put64(p, tpoff(sym_target(false)) + uint64_t(A)); break;
            case R_X86_64_GOTTPOFF:
                pc32(got_base_ + 8 * got_index_.at("t" + std::to_string(sym_target(false).value)));
                break;
            case R_X86_64_TLSGD:
                pc32(got_base_ + 8 * got_index_.at("g" + std::to_string(sym_target(false).value)));
                break;
            case R_X86_64_TLSLD: pc32(got_base_ + 8 * got_index_.at("l")); break;
            case R_X86_64_DTPOFF32: {
                const Target t = sym_target(false);
                if (t.kind != Target::Tls) throw LinkError("TLS access to a variable that isn't thread_local");
                signed32(int64_t(t.value) + A);
                break;
            }
            case R_X86_64_DTPOFF64: {
                const Target t = sym_target(false);
                if (t.kind != Target::Tls) throw LinkError("TLS access to a variable that isn't thread_local");
                put64(p, t.value + uint64_t(A));
                break;
            }
            case R_X86_64_DTPMOD64: put64(p, tls_module()); break;
            default:
                throw LinkError("unsupported relocation type " + std::to_string(type) + " in " + sec.name + " of " + file_of(ob));
            }
        });

        for (size_t i = 0; i < got_.size(); ++i)
            put64(buffer_at(got_base_ + 8 * i, 8), got_[i].is_value ? got_[i].value : address(got_[i].target));
    }

    void make_sites_and_delta() {
        std::set<uint64_t> redirected; // entries already handled (aliases share an address)
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            for (size_t k = 0; k < ob.symbols.size(); ++k) {
                const ObjSymbol& s = ob.symbols[k];
                if (!named(s) || !defined_here(o, s)) continue;
                const SecState& ss = st_[o].secs[s.shndx];
                if (!loaded(ss)) continue;
                const SymbolKey key = key_of(ob, s);
                const IndexEntry* e = st_[o].existing[k];
                const uint64_t addr = ss.addr + s.value;

                if (ss.kind != SecKind::Text) {
                    if (!e && s.type != STT_NOTYPE) {
                        IndexEntry ne;
                        ne.kind = SymbolKind::Data;
                        ne.address = addr;
                        ne.size = s.size;
                        ne.writable = is_data(ss.kind);
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
                    if (!is_initializer_function(s.name)) ++out_.functions_new;
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

    void emit() {
        if (text_size_) out_.patch.segments.push_back({text_start_, text_size_, proto::kProtRead | proto::kProtExec, text_});
        if (ro_size_) out_.patch.segments.push_back({ro_start_, ro_size_, proto::kProtRead, ro_});
        if (data_size_) out_.patch.segments.push_back({data_start_, data_size_, proto::kProtRead | proto::kProtWrite, data_});
        if (eh_frame_) out_.patch.eh_frames.push_back(eh_frame_);
        for (const Target& t : run_) out_.patch.initializers.push_back(address(t));
        out_.bytes_used = data_start_ + data_size_ - opt_.region_cursor;
    }

    // Where section i of object o lives for the debugger: its patch address, or the
    // old address for data bound to the running program.
    uint64_t debug_address(size_t o, uint32_t i) const {
        const SecState& ss = st_[o].secs[i];
        if (ss.discarded) {
            auto k = kept_copy_.find({o, i});
            return k == kept_copy_.end() ? 0 : debug_address(k->second.first, k->second.second);
        }
        if (loaded(ss)) return ss.addr;
        if (ss.bound_old && ss.kind != SecKind::Tls) return ss.old_base;
        return 0;
    }

    // Value of symbol k as the debugger sees it, never throwing: debug info may
    // mention symbols the patch doesn't otherwise use.
    uint64_t debug_symbol_value(size_t o, uint32_t k) const {
        const ObjSymbol& s = obj(o).symbols[k];
        if (s.shndx == SHN_ABS) return s.value;
        if (s.defined() && s.shndx < SHN_LORESERVE) {
            const SecState& ss = st_[o].secs[s.shndx];
            if (ss.kind == SecKind::Tls && ss.bound_old) return ss.old_base + s.value;
            return debug_address(o, s.shndx) + (s.type == STT_SECTION ? 0 : s.value);
        }
        if (s.bind == STB_LOCAL) return 0;
        if (const IndexEntry* e = index_.find({false, "", s.name})) return e->address;
        auto x = external_.find(s.name);
        return x != external_.end() ? x->second : 0;
    }

    // A copy of each object with allocated sections' sh_addr set to where they live,
    // their contents replaced by the final bytes, debug sections relocated, and every
    // relocation section disabled so the debugger doesn't apply them again.
    void build_debug_objects() {
        for (size_t o = 0; o < st_.size(); ++o) {
            const ElfObject& ob = obj(o);
            const elf::ElfFile& f = ob.file();
            std::vector<uint8_t> img = f.bytes();
            const uint64_t shoff = f.header().shoff;
            for (uint32_t i = 0; i < f.sections().size(); ++i) {
                const Shdr& sh = f.sections()[i];
                const uint64_t hdr = shoff + uint64_t(i) * sizeof(Shdr);
                if (sh.flags & SHF_ALLOC) {
                    const uint64_t addr = debug_address(o, i);
                    std::memcpy(img.data() + hdr + offsetof(Shdr, addr), &addr, 8);
                    const SecState& ss = st_[o].secs[i];
                    if (loaded(ss) && ss.kind != SecKind::Bss && sh.type != SHT_NOBITS)
                        std::memcpy(img.data() + sh.offset, buffer_at(ss.addr, sh.size), sh.size);
                }
                if (sh.type != SHT_RELA) continue;
                if (sh.info < f.sections().size() && !(f.sections()[sh.info].flags & SHF_ALLOC))
                    relocate_debug_section(o, sh.info, img);
                const uint32_t null_type = SHT_NULL;
                std::memcpy(img.data() + hdr + offsetof(Shdr, type), &null_type, 4);
            }
            out_.patch.debug_objects.push_back(std::move(img));
        }
    }

    void relocate_debug_section(size_t o, uint32_t target, std::vector<uint8_t>& img) const {
        const ElfObject& ob = obj(o);
        const Shdr& sh = ob.file().sections()[target];
        for (const Rela& r : ob.sections[target].relocs) {
            const uint32_t type = r.type();
            const uint64_t width = type == R_X86_64_64 || type == R_X86_64_DTPOFF64 ? 8 : 4;
            if (r.offset > sh.size || width > sh.size - r.offset) continue;
            uint8_t* p = img.data() + sh.offset + r.offset;
            const uint64_t v = debug_symbol_value(o, r.sym()) + uint64_t(r.addend);
            switch (type) {
            case R_X86_64_64: case R_X86_64_DTPOFF64: put64(p, v); break;
            case R_X86_64_32: case R_X86_64_32S: case R_X86_64_DTPOFF32: put32(p, uint32_t(v)); break;
            default: break; // nothing else appears in debug sections
            }
        }
    }

    const SymbolIndex& index_;
    const ExternalResolver& resolve_;
    LinkOptions opt_;
    std::vector<ObjState> st_;
    std::map<std::pair<size_t, uint32_t>, std::pair<size_t, uint32_t>> kept_copy_;
    // Text section -> (offset, canonical address) of the existing functions it defines.
    std::map<std::pair<size_t, uint32_t>, std::vector<std::pair<uint64_t, uint64_t>>> canonical_;
    std::unordered_map<std::string, std::pair<size_t, size_t>> globals_;
    std::unordered_map<std::string, uint64_t> external_;
    std::unordered_map<std::string, size_t> got_index_;
    std::vector<GotSlot> got_;
    std::unordered_map<uint64_t, size_t> stub_index_;
    std::vector<uint64_t> stub_targets_;
    std::vector<Target> run_; // initializers to call
    uint64_t text_start_ = 0, text_size_ = 0, stub_base_ = 0;
    uint64_t ro_start_ = 0, ro_size_ = 0, got_base_ = 0, eh_frame_ = 0;
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
