#include "host/image/dwarf_reader.h"

namespace hotpp::dwarf {

using elf::ElfError;

namespace {
// clang-format off
enum Form : uint64_t {
    DW_FORM_addr = 0x01, DW_FORM_block2 = 0x03, DW_FORM_block4 = 0x04, DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06, DW_FORM_data8 = 0x07, DW_FORM_string = 0x08, DW_FORM_block = 0x09,
    DW_FORM_block1 = 0x0a, DW_FORM_data1 = 0x0b, DW_FORM_flag = 0x0c, DW_FORM_sdata = 0x0d,
    DW_FORM_strp = 0x0e, DW_FORM_udata = 0x0f, DW_FORM_ref_addr = 0x10, DW_FORM_ref1 = 0x11,
    DW_FORM_ref2 = 0x12, DW_FORM_ref4 = 0x13, DW_FORM_ref8 = 0x14, DW_FORM_ref_udata = 0x15,
    DW_FORM_indirect = 0x16, DW_FORM_sec_offset = 0x17, DW_FORM_exprloc = 0x18,
    DW_FORM_flag_present = 0x19, DW_FORM_strx = 0x1a, DW_FORM_addrx = 0x1b,
    DW_FORM_ref_sup4 = 0x1c, DW_FORM_strp_sup = 0x1d, DW_FORM_data16 = 0x1e,
    DW_FORM_line_strp = 0x1f, DW_FORM_ref_sig8 = 0x20, DW_FORM_implicit_const = 0x21,
    DW_FORM_loclistx = 0x22, DW_FORM_rnglistx = 0x23, DW_FORM_ref_sup8 = 0x24,
    DW_FORM_strx1 = 0x25, DW_FORM_strx2 = 0x26, DW_FORM_strx3 = 0x27, DW_FORM_strx4 = 0x28,
    DW_FORM_addrx1 = 0x29, DW_FORM_addrx2 = 0x2a, DW_FORM_addrx3 = 0x2b, DW_FORM_addrx4 = 0x2c,
};
// clang-format on

Span section(const elf::ElfFile& f, const char* name) {
    const int idx = f.find_section(name);
    if (idx < 0) return {};
    const elf::Shdr& s = f.sections()[size_t(idx)];
    if (s.type == elf::SHT_NULL || s.type == elf::SHT_NOBITS) return {};
    if (s.flags & elf::SHF_COMPRESSED)
        throw ElfError(std::string("compressed ") + name + " is not supported; build without -gz");
    return {f.section_data(size_t(idx)), s.size};
}

std::string string_in(Span s, uint64_t offset) {
    Cursor c(s);
    c.seek(offset);
    return c.cstr();
}
} // namespace

void Cursor::fail() { throw ElfError("malformed DWARF"); }

void Cursor::seek(uint64_t p) {
    if (p > s_.size) fail();
    pos_ = p;
}

void Cursor::skip(uint64_t n) {
    if (n > s_.size - pos_) fail();
    pos_ += n;
}

uint64_t Cursor::fixed(int n) {
    if (uint64_t(n) > s_.size - pos_) fail();
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(s_.data[pos_ + uint64_t(i)]) << (8 * i);
    pos_ += uint64_t(n);
    return v;
}

uint64_t Cursor::uleb() {
    uint64_t v = 0;
    for (int shift = 0;; shift += 7) {
        const uint8_t b = uint8_t(fixed(1));
        if (shift < 64) v |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
    }
}

int64_t Cursor::sleb() {
    int64_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = uint8_t(fixed(1));
        if (shift < 64) v |= int64_t(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(int64_t(1) << shift);
    return v;
}

std::string Cursor::cstr() {
    const uint64_t start = pos_;
    while (pos_ < s_.size && s_.data[pos_]) ++pos_;
    if (pos_ >= s_.size) fail();
    std::string out(reinterpret_cast<const char*>(s_.data + start), pos_ - start);
    ++pos_;
    return out;
}

Span Cursor::bytes(uint64_t n) {
    const uint64_t start = pos_;
    skip(n);
    return {s_.data + start, n};
}

Sections Sections::from(const elf::ElfFile& f) {
    Sections s;
    s.info = section(f, ".debug_info");
    s.abbrev = section(f, ".debug_abbrev");
    s.str = section(f, ".debug_str");
    s.line_str = section(f, ".debug_line_str");
    s.str_offsets = section(f, ".debug_str_offsets");
    return s;
}

AbbrevTable parse_abbrevs(Span abbrev, uint64_t offset) {
    AbbrevTable table;
    Cursor a(abbrev);
    a.seek(offset);
    for (;;) {
        const uint64_t code = a.uleb();
        if (code == 0) break;
        Abbrev ab;
        ab.tag = a.uleb();
        ab.children = a.fixed(1) != 0;
        for (;;) {
            AttrSpec spec;
            spec.attr = a.uleb();
            spec.form = a.uleb();
            if (spec.attr == 0 && spec.form == 0) break;
            if (spec.form == DW_FORM_implicit_const) spec.implicit = a.sleb();
            ab.attrs.push_back(spec);
        }
        table.emplace(code, std::move(ab));
    }
    return table;
}

std::vector<Unit> units(const Sections& s) {
    std::vector<Unit> out;
    if (!s.info.data) return out;
    Cursor c(s.info);
    while (c.pos() < s.info.size) {
        Unit u;
        uint64_t length = c.fixed(4);
        if (length == 0xffffffff) {
            length = c.fixed(8);
            u.offset_size = 8;
        }
        if (length > s.info.size - c.pos()) Cursor::fail();
        u.end = c.pos() + length;
        u.version = uint16_t(c.fixed(2));
        u.unit_type = DW_UT_compile;
        if (u.version >= 5) {
            u.unit_type = uint8_t(c.fixed(1));
            u.addr_size = uint8_t(c.fixed(1));
            u.abbrev_offset = c.fixed(u.offset_size);
            if (u.unit_type == 0x02 || u.unit_type == 0x06) c.skip(8 + uint64_t(u.offset_size)); // type units
            else if (u.unit_type == 0x04 || u.unit_type == 0x05) c.skip(8);                     // skeleton/split
        } else if (u.version >= 2) {
            u.abbrev_offset = c.fixed(u.offset_size);
            u.addr_size = uint8_t(c.fixed(1));
        } else {
            Cursor::fail();
        }
        u.die_offset = c.pos();
        out.push_back(u);
        c.seek(u.end);
    }
    return out;
}

Value read_value(Cursor& c, const AttrSpec& spec, const Unit& u, const Sections& s) {
    Value v;
    uint64_t form = spec.form;
    if (form == DW_FORM_indirect) form = c.uleb();
    auto unsigned_value = [&](uint64_t x) {
        v.kind = Value::Unsigned;
        v.u = x;
    };
    switch (form) {
    case DW_FORM_string: v.kind = Value::String; v.str = c.cstr(); break;
    case DW_FORM_strp: {
        const uint64_t off = c.fixed(u.offset_size);
        if (s.str.data) { v.kind = Value::String; v.str = string_in(s.str, off); }
        break;
    }
    case DW_FORM_line_strp: {
        const uint64_t off = c.fixed(u.offset_size);
        if (s.line_str.data) { v.kind = Value::String; v.str = string_in(s.line_str, off); }
        break;
    }
    case DW_FORM_strx: v.kind = Value::StrIndex; v.u = c.uleb(); break;
    case DW_FORM_strx1: v.kind = Value::StrIndex; v.u = c.fixed(1); break;
    case DW_FORM_strx2: v.kind = Value::StrIndex; v.u = c.fixed(2); break;
    case DW_FORM_strx3: v.kind = Value::StrIndex; v.u = c.fixed(3); break;
    case DW_FORM_strx4: v.kind = Value::StrIndex; v.u = c.fixed(4); break;
    case DW_FORM_data1: case DW_FORM_ref1: case DW_FORM_flag: case DW_FORM_addrx1: unsigned_value(c.fixed(1)); break;
    case DW_FORM_data2: case DW_FORM_ref2: case DW_FORM_addrx2: unsigned_value(c.fixed(2)); break;
    case DW_FORM_addrx3: unsigned_value(c.fixed(3)); break;
    case DW_FORM_data4: case DW_FORM_ref4: case DW_FORM_ref_sup4: case DW_FORM_addrx4: unsigned_value(c.fixed(4)); break;
    case DW_FORM_data8: case DW_FORM_ref8: case DW_FORM_ref_sig8: case DW_FORM_ref_sup8: unsigned_value(c.fixed(8)); break;
    case DW_FORM_addr: unsigned_value(c.fixed(u.addr_size)); break;
    case DW_FORM_sec_offset: case DW_FORM_ref_addr: case DW_FORM_strp_sup: unsigned_value(c.fixed(u.offset_size)); break;
    case DW_FORM_udata: case DW_FORM_ref_udata: case DW_FORM_addrx: case DW_FORM_loclistx: case DW_FORM_rnglistx:
        unsigned_value(c.uleb());
        break;
    case DW_FORM_sdata: v.kind = Value::Signed; v.s = c.sleb(); break;
    case DW_FORM_implicit_const: v.kind = Value::Signed; v.s = spec.implicit; break;
    case DW_FORM_flag_present: unsigned_value(1); break;
    case DW_FORM_data16: c.skip(16); break;
    case DW_FORM_block1: v.kind = Value::Block; v.block = c.bytes(c.fixed(1)); break;
    case DW_FORM_block2: v.kind = Value::Block; v.block = c.bytes(c.fixed(2)); break;
    case DW_FORM_block4: v.kind = Value::Block; v.block = c.bytes(c.fixed(4)); break;
    case DW_FORM_block: case DW_FORM_exprloc: v.kind = Value::Block; v.block = c.bytes(c.uleb()); break;
    default: throw ElfError("unsupported DWARF form " + std::to_string(form));
    }
    return v;
}

std::string string_at_index(const Sections& s, const Unit& u, uint64_t base, uint64_t index) {
    if (!s.str_offsets.data || !s.str.data) return {};
    Cursor o(s.str_offsets);
    o.seek(base + index * uint64_t(u.offset_size));
    return string_in(s.str, o.fixed(u.offset_size));
}

} // namespace hotpp::dwarf
