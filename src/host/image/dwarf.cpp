// DWARF readers used by the host: which source files the executable was built from
// (compile-unit names) and the layout of every named struct/class (layout warnings).
#include "host/image/dwarf_types.h"
#include "host/image/elf_image.h"
#include "host/image/dwarf_reader.h"

#include <optional>

namespace hotpp::image {

using namespace hotpp::dwarf;

std::vector<std::filesystem::path> dwarf_compile_units(const elf::ElfFile& f) {
    const Sections s = Sections::from(f);
    std::vector<std::filesystem::path> out;
    for (const Unit& u : units(s)) {
        if (u.unit_type != DW_UT_compile || !s.abbrev.data) continue;
        const AbbrevTable table = parse_abbrevs(s.abbrev, u.abbrev_offset);
        Cursor c(s.info, u.die_offset);
        auto it = table.find(c.uleb());
        if (it == table.end()) Cursor::fail();
        Value name, comp_dir;
        uint64_t str_base = 8; // DWARF 5 header size of a 32-bit .debug_str_offsets
        for (const AttrSpec& spec : it->second.attrs) {
            Value v = read_value(c, spec, u, s);
            if (spec.attr == DW_AT_name) name = std::move(v);
            else if (spec.attr == DW_AT_comp_dir) comp_dir = std::move(v);
            else if (spec.attr == DW_AT_str_offsets_base) str_base = v.u;
        }
        auto text = [&](const Value& v) {
            return v.kind == Value::StrIndex ? string_at_index(s, u, str_base, v.u) : v.str;
        };
        const std::string n = text(name);
        if (n.empty()) continue;
        std::filesystem::path p(n);
        if (p.is_relative()) p = std::filesystem::path(text(comp_dir)) / p;
        out.push_back(p.lexically_normal());
    }
    return out;
}

namespace {

// DW_AT_data_member_location as a constant, or a DWARF 2 style DW_OP_plus_uconst.
std::optional<int64_t> member_offset(const Value& v) {
    if (v.is_constant()) return v.constant();
    if (v.kind == Value::Block && v.block.size >= 1 && v.block.data[0] == 0x23) {
        Cursor c(v.block, 1);
        return int64_t(c.uleb());
    }
    return std::nullopt;
}

struct Scope {
    uint64_t tag = 0;
    std::string name;
    bool local = false;                // inside a function, or unnamed: not identifiable
    std::optional<TypeLayout> layout;  // the struct being collected, if any
    int bases = 0;
};

} // namespace

std::unordered_map<std::string, TypeLayout> dwarf_type_layouts(const elf::ElfFile& f) {
    const Sections s = Sections::from(f);
    std::unordered_map<std::string, TypeLayout> out;
    std::unordered_map<uint64_t, AbbrevTable> tables;
    for (const Unit& u : units(s)) {
        if (u.unit_type != DW_UT_compile || !s.abbrev.data) continue;
        auto t = tables.find(u.abbrev_offset);
        if (t == tables.end()) t = tables.emplace(u.abbrev_offset, parse_abbrevs(s.abbrev, u.abbrev_offset)).first;
        const AbbrevTable& table = t->second;

        Cursor c(s.info, u.die_offset);
        uint64_t str_base = 8;
        std::vector<Scope> scopes;
        bool first = true;
        while (c.pos() < u.end) {
            const uint64_t code = c.uleb();
            if (code == 0) {
                if (scopes.empty()) break;
                Scope done = std::move(scopes.back());
                scopes.pop_back();
                if (done.layout) {
                    std::string key;
                    for (const Scope& sc : scopes)
                        if (!sc.name.empty()) key += sc.name + "::";
                    key += done.name;
                    out.emplace(std::move(key), std::move(*done.layout));
                }
                continue;
            }
            auto it = table.find(code);
            if (it == table.end()) Cursor::fail();
            const Abbrev& ab = it->second;

            Value name, size, location, bit_offset, declaration;
            for (const AttrSpec& spec : ab.attrs) {
                Value v = read_value(c, spec, u, s);
                switch (spec.attr) {
                case DW_AT_name: name = std::move(v); break;
                case DW_AT_byte_size: size = std::move(v); break;
                case DW_AT_data_member_location: location = std::move(v); break;
                case DW_AT_data_bit_offset: bit_offset = std::move(v); break;
                case DW_AT_declaration: declaration = std::move(v); break;
                case DW_AT_str_offsets_base: if (first) str_base = v.u; break;
                default: break;
                }
            }
            first = false;
            const std::string n = name.kind == Value::StrIndex ? string_at_index(s, u, str_base, name.u) : name.str;

            const bool in_local = !scopes.empty() && scopes.back().local;
            Scope* parent = scopes.empty() ? nullptr : &scopes.back();
            if (parent && parent->layout && (ab.tag == DW_TAG_member || ab.tag == DW_TAG_inheritance)) {
                // A data member (static members have no location) or a base class.
                std::optional<int64_t> off = member_offset(location);
                if (bit_offset.is_constant()) off = bit_offset.constant(); // bit-fields: bits, not bytes
                if (off) {
                    const std::string m = ab.tag == DW_TAG_inheritance ? "<base " + std::to_string(parent->bases++) + ">"
                                                                       : (n.empty() ? "<anonymous>" : n);
                    parent->layout->members.emplace_back(m, *off);
                }
            }
            if (!ab.children) continue;

            Scope sc;
            sc.tag = ab.tag;
            sc.name = ab.tag == DW_TAG_namespace && n.empty() ? "(anonymous namespace)" : n;
            const bool type = ab.tag == DW_TAG_structure_type || ab.tag == DW_TAG_class_type || ab.tag == DW_TAG_union_type;
            const bool container = type || ab.tag == DW_TAG_namespace || ab.tag == DW_TAG_compile_unit;
            sc.local = in_local || !container || (type && n.empty());
            if (ab.tag == DW_TAG_compile_unit) sc.name.clear();
            if (type && !sc.local && !declaration.is_constant() && size.is_constant())
                sc.layout = TypeLayout{uint64_t(size.constant()), {}};
            scopes.push_back(std::move(sc));
        }
    }
    return out;
}

} // namespace hotpp::image
