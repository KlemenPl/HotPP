// Low-level DWARF 4/5 reading shared by the compile-unit and type readers:
// bounds-checked cursor, abbreviation tables, unit headers and attribute values.
#pragma once

#include "host/elf/elf_file.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hotpp::dwarf {

// clang-format off
enum Tag : uint64_t {
    DW_TAG_class_type = 0x02, DW_TAG_lexical_block = 0x0b, DW_TAG_member = 0x0d,
    DW_TAG_compile_unit = 0x11, DW_TAG_structure_type = 0x13, DW_TAG_union_type = 0x17,
    DW_TAG_inheritance = 0x1c, DW_TAG_subprogram = 0x2e, DW_TAG_namespace = 0x39,
};
enum Attr : uint64_t {
    DW_AT_name = 0x03, DW_AT_byte_size = 0x0b, DW_AT_comp_dir = 0x1b, DW_AT_declaration = 0x3c,
    DW_AT_data_member_location = 0x38, DW_AT_data_bit_offset = 0x6b, DW_AT_bit_size = 0x0d,
    DW_AT_str_offsets_base = 0x72,
};
// clang-format on

struct Span {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
};

class Cursor {
public:
    explicit Cursor(Span s, uint64_t pos = 0) : s_(s), pos_(pos) {}
    uint64_t pos() const { return pos_; }
    void seek(uint64_t p);
    void skip(uint64_t n);
    uint64_t fixed(int n);
    uint64_t uleb();
    int64_t sleb();
    std::string cstr();
    Span bytes(uint64_t n);

    [[noreturn]] static void fail();

private:
    Span s_;
    uint64_t pos_;
};

struct Sections {
    Span info, abbrev, str, line_str, str_offsets;
    static Sections from(const elf::ElfFile& file); // throws on compressed debug sections
};

struct AttrSpec {
    uint64_t attr = 0;
    uint64_t form = 0;
    int64_t implicit = 0; // DW_FORM_implicit_const
};

struct Abbrev {
    uint64_t tag = 0;
    bool children = false;
    std::vector<AttrSpec> attrs;
};

using AbbrevTable = std::unordered_map<uint64_t, Abbrev>;
AbbrevTable parse_abbrevs(Span abbrev, uint64_t offset);

struct Unit {
    uint64_t end = 0;        // offset just past the unit
    uint64_t die_offset = 0; // first DIE
    uint16_t version = 0;
    uint8_t unit_type = 0; // DW_UT_* (DWARF 5); compile for older versions
    uint8_t addr_size = 8;
    int offset_size = 4;
    uint64_t abbrev_offset = 0;
};
constexpr uint8_t DW_UT_compile = 0x01;

// All units of .debug_info, in order.
std::vector<Unit> units(const Sections& s);

struct Value {
    enum Kind { None, Unsigned, Signed, String, StrIndex, Block } kind = None;
    uint64_t u = 0;
    int64_t s = 0;
    std::string str;
    Span block;

    bool is_constant() const { return kind == Unsigned || kind == Signed; }
    int64_t constant() const { return kind == Signed ? s : int64_t(u); }
};

// Reads one attribute value; strp/line_strp strings are resolved, strx forms are
// returned as StrIndex (resolve with string_at_index once DW_AT_str_offsets_base is known).
Value read_value(Cursor& c, const AttrSpec& spec, const Unit& u, const Sections& s);
std::string string_at_index(const Sections& s, const Unit& u, uint64_t str_offsets_base, uint64_t index);

} // namespace hotpp::dwarf
