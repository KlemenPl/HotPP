// Links tests/unit/data/link_sample.cpp against a hand-made index of a pretend
// running program and checks every binding decision in the produced bytes.
#include "host/link/linker.h"
#include "host/object/elf_object.h"
#include "unit/test.h"

#include <cstring>

using namespace hotpp;
using namespace hotpp::link;

namespace {

constexpr uint64_t kCounter = 0x10000000;   // old `counter`
constexpr uint64_t kTicks = 0x10000010;     // old `ticks` (file-local)
constexpr uint64_t kValue = 0x10001005;     // old value()
constexpr uint64_t kUpdate = 0x10002005;    // old update()
constexpr uint64_t kPrintf = 0x7f00deadb000; // printf in a shared library
constexpr uint64_t kRegion = 0x20000000;

IndexEntry data(uint64_t addr) {
    IndexEntry e;
    e.kind = SymbolKind::Data;
    e.address = addr;
    e.size = 4;
    e.writable = true;
    return e;
}

IndexEntry function(uint64_t addr) {
    IndexEntry e;
    e.kind = SymbolKind::Function;
    e.address = addr;
    SiteLayout site;
    site.short_jump[1] = 0xF9;
    e.versions.push_back({addr, site});
    return e;
}

struct Fixture {
    std::unique_ptr<object::ElfObject> obj = object::ElfObject::load(HOTPP_TEST_LINK_OBJECT);
    SymbolIndex index;
    std::vector<std::string> asked;
    LinkResult result;

    Fixture() {
        index.add({false, "", "counter"}, data(kCounter));
        index.add({true, obj->source_name, "_ZL5ticks"}, data(kTicks));
        index.add({false, "", "_Z5valuev"}, function(kValue));
        index.add({false, "", "_Z6updatev"}, function(kUpdate));
        LinkOptions opt;
        opt.region_cursor = kRegion;
        opt.region_end = kRegion + (16 << 20);
        result = link_patch({obj.get()}, index,
                            [this](const std::vector<std::string>& names) {
                                asked = names;
                                std::vector<uint64_t> out;
                                for (const auto& n : names) out.push_back(n == "printf" ? kPrintf : 0);
                                return out;
                            },
                            opt);
    }

    // Bytes at a patch address, from whichever segment holds it.
    const uint8_t* at(uint64_t addr, size_t n) const {
        for (const auto& seg : result.patch.segments)
            if (addr >= seg.addr && addr + n <= seg.addr + seg.bytes.size()) return seg.bytes.data() + (addr - seg.addr);
        return nullptr;
    }

    uint64_t new_entry(const std::string& name) const {
        for (const auto& v : result.delta.versions)
            if (v.key.name == name) return v.version.entry;
        for (const auto& s : result.delta.symbols)
            if (s.key.name == name) return s.entry.address;
        return 0;
    }

    const object::ObjSection* section(const std::string& name, uint32_t* index_out = nullptr) const {
        for (uint32_t i = 0; i < obj->sections.size(); ++i)
            if (obj->sections[i].name == name) {
                if (index_out) *index_out = i;
                return &obj->sections[i];
            }
        return nullptr;
    }

    // Where each rel32 relocation in the function's section ends up pointing, by symbol name.
    std::vector<std::pair<std::string, uint64_t>> rel32_targets(const std::string& fn) const {
        std::vector<std::pair<std::string, uint64_t>> out;
        const object::ObjSection* sec = section(".text." + fn);
        const uint64_t base = new_entry(fn) - 5; // the symbol sits after the 5 padding bytes
        if (!sec) return out;
        for (const auto& r : sec->relocs) {
            if (r.type() != elf::R_X86_64_PC32 && r.type() != elf::R_X86_64_PLT32 &&
                r.type() != elf::R_X86_64_GOTPCREL && r.type() != elf::R_X86_64_REX_GOTPCRELX)
                continue;
            const uint8_t* p = at(base + r.offset, 4);
            if (!p) continue;
            int32_t v;
            std::memcpy(&v, p, 4);
            // S + A - P was stored; recover S (A is -4 or the like for RIP-relative operands).
            const uint64_t target = base + r.offset + uint64_t(int64_t(v)) - uint64_t(r.addend);
            const auto& s = obj->symbols[r.sym()];
            out.emplace_back(s.type == elf::STT_SECTION ? obj->sections[s.shndx].name : s.name, target);
        }
        return out;
    }
};

} // namespace

HOTPP_TEST(linker_keeps_existing_variables) {
    Fixture f;
    CHECK(f.result.data_kept == 2);
    int seen = 0;
    for (const auto& [name, target] : f.rel32_targets("_Z6updatev")) {
        if (name == "counter") { CHECK(target == kCounter); ++seen; }
        if (name == ".bss._ZL5ticks" || name == "_ZL5ticks") { CHECK(target == kTicks); ++seen; }
    }
    CHECK(seen >= 2);
}

HOTPP_TEST(linker_redirects_existing_functions) {
    Fixture f;
    CHECK(f.result.functions_redirected == 2);
    CHECK(f.result.functions_new == 2); // fresh(), value_pointer()
    CHECK(f.result.patch.sites.size() == 2);
    for (const auto& site : f.result.patch.sites) {
        CHECK(site.pad_addr == site.entry_addr - 5);
        CHECK(site.entry_bytes[0] == 0xEB && site.entry_bytes[1] == 0xF9);
        if (site.entry_addr == kValue) CHECK(site.target == f.new_entry("_Z5valuev"));
        else CHECK(site.entry_addr == kUpdate && site.target == f.new_entry("_Z6updatev"));
    }
    // The new copies are padded too, so a later patch can redirect them again.
    for (const auto& v : f.result.delta.versions) CHECK(v.version.site.has_value());
}

HOTPP_TEST(linker_calls_libraries_through_stubs) {
    Fixture f;
    CHECK(f.asked.size() == 1 && f.asked[0] == "printf");
    bool found = false;
    for (const auto& [name, target] : f.rel32_targets("_Z6updatev")) {
        if (name != "printf") continue;
        found = true;
        const uint8_t* stub = f.at(target, 14);
        CHECK(stub && stub[0] == 0xFF && stub[1] == 0x25); // jmp *0(%rip)
        uint64_t dest = 0;
        if (stub) std::memcpy(&dest, stub + 6, 8);
        CHECK(dest == kPrintf);
    }
    CHECK(found);
}

HOTPP_TEST(linker_takes_canonical_function_addresses) {
    // &value inside the patch must equal the address the old code has for value().
    Fixture f;
    bool found = false;
    for (const auto& [name, target] : f.rel32_targets("_Z13value_pointerv")) {
        if (name != "_Z5valuev") continue;
        found = true;
        // Either a direct lea of the canonical address or a GOT slot holding it.
        const uint8_t* slot = f.at(target, 8);
        uint64_t via_got = 0;
        if (slot) std::memcpy(&via_got, slot, 8);
        CHECK(target == kValue || via_got == kValue);
    }
    CHECK(found);
}

HOTPP_TEST(linker_rejects_resized_variables) {
    auto obj = object::ElfObject::load(HOTPP_TEST_LINK_OBJECT);
    SymbolIndex index;
    IndexEntry wide = data(kCounter);
    wide.size = 8; // the running program has an 8-byte counter
    index.add({false, "", "counter"}, wide);
    LinkOptions opt;
    opt.region_cursor = kRegion;
    opt.region_end = kRegion + (16 << 20);
    bool restart = false;
    try {
        link_patch({obj.get()}, index, [](const std::vector<std::string>& n) { return std::vector<uint64_t>(n.size(), 1); }, opt);
    } catch (const RestartRequired&) {
        restart = true;
    }
    CHECK(restart);
}
