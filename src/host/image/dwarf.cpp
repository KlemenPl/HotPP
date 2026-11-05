// Just enough DWARF 4/5 to read each compile unit's DW_AT_name and DW_AT_comp_dir,
// which tells the host which source files the executable was built from.
#include "host/image/elf_image.h"

#include <optional>

namespace hotpp::image {

using namespace hotpp::elf;

namespace {

constexpr uint16_t DW_AT_name = 0x03, DW_AT_comp_dir = 0x1b, DW_AT_str_offsets_base = 0x72;
constexpr uint8_t DW_UT_compile = 0x01;

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

struct Span {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
};

class Cursor {
public:
    Cursor(Span s, uint64_t pos = 0) : s_(s), pos_(pos) {}
    uint64_t pos() const { return pos_; }
    void seek(uint64_t p) {
        if (p > s_.size) fail();
        pos_ = p;
    }
    void skip(uint64_t n) {
        if (n > s_.size - pos_) fail();
        pos_ += n;
    }
    uint64_t fixed(int n) {
        if (uint64_t(n) > s_.size - pos_) fail();
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint64_t(s_.data[pos_ + i]) << (8 * i);
        pos_ += uint64_t(n);
        return v;
    }
    uint64_t uleb() {
        uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            uint8_t b = uint8_t(fixed(1));
            if (shift < 64) v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
    }
    int64_t sleb() {
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
    std::string cstr() {
        const uint64_t start = pos_;
        while (pos_ < s_.size && s_.data[pos_]) ++pos_;
        if (pos_ >= s_.size) fail();
        std::string out(reinterpret_cast<const char*>(s_.data + start), pos_ - start);
        ++pos_;
        return out;
    }

    [[noreturn]] static void fail() { throw ElfError("malformed DWARF in .debug_info"); }

private:
    Span s_;
    uint64_t pos_;
};

Span section_span(const ElfFile& f, const char* name) {
    int idx = f.find_section(name);
    if (idx < 0) return {};
    const Shdr& s = f.sections()[size_t(idx)];
    if (s.flags & SHF_COMPRESSED)
        throw ElfError(std::string("compressed ") + name + " is not supported; build without -gz");
    return {f.section_data(size_t(idx)), s.size};
}

std::string string_in(Span s, uint64_t offset) {
    Cursor c(s);
    c.seek(offset);
    return c.cstr();
}

// A string attribute value, resolved once the whole DIE has been read (DW_FORM_strx
// needs DW_AT_str_offsets_base, which may come after the name).
struct PendingString {
    std::optional<std::string> value;
    std::optional<uint64_t> strx;
};

} // namespace

std::vector<std::filesystem::path> dwarf_compile_units(const ElfFile& f) {
    const Span info = section_span(f, ".debug_info");
    if (!info.data) return {};
    const Span abbrev = section_span(f, ".debug_abbrev");
    const Span str = section_span(f, ".debug_str");
    const Span line_str = section_span(f, ".debug_line_str");
    const Span str_offsets = section_span(f, ".debug_str_offsets");

    std::vector<std::filesystem::path> units;
    Cursor c(info);
    while (c.pos() < info.size) {
        const uint64_t unit_start = c.pos();
        uint64_t length = c.fixed(4);
        int offset_size = 4;
        if (length == 0xffffffff) {
            length = c.fixed(8);
            offset_size = 8;
        }
        const uint64_t unit_end = c.pos() + length;
        if (unit_end > info.size || unit_end < c.pos()) Cursor::fail();
        const uint16_t version = uint16_t(c.fixed(2));
        uint64_t abbrev_offset = 0;
        uint8_t addr_size = 8;
        uint8_t unit_type = DW_UT_compile;
        if (version >= 5) {
            unit_type = uint8_t(c.fixed(1));
            addr_size = uint8_t(c.fixed(1));
            abbrev_offset = c.fixed(offset_size);
        } else if (version >= 2) {
            abbrev_offset = c.fixed(offset_size);
            addr_size = uint8_t(c.fixed(1));
        } else {
            Cursor::fail();
        }
        if (unit_type != DW_UT_compile || !abbrev.data) {
            c.seek(unit_end);
            continue;
        }
        (void)unit_start;

        // First DIE of the unit and its abbreviation.
        const uint64_t code = c.uleb();
        Cursor a(abbrev, 0);
        a.seek(abbrev_offset);
        std::vector<std::pair<uint64_t, uint64_t>> specs; // (attribute, form)
        std::vector<int64_t> implicit;
        for (;;) {
            const uint64_t ac = a.uleb();
            if (ac == 0) Cursor::fail();
            a.uleb();   // tag
            a.fixed(1); // has children
            specs.clear();
            implicit.clear();
            for (;;) {
                const uint64_t at = a.uleb(), form = a.uleb();
                if (at == 0 && form == 0) break;
                specs.emplace_back(at, form);
                implicit.push_back(form == DW_FORM_implicit_const ? a.sleb() : 0);
            }
            if (ac == code) break;
        }

        PendingString name, comp_dir;
        uint64_t str_offsets_base = 8; // DWARF 5 header size of a 32-bit .debug_str_offsets
        for (size_t i = 0; i < specs.size(); ++i) {
            const uint64_t at = specs[i].first;
            uint64_t form = specs[i].second;
            if (form == DW_FORM_indirect) form = c.uleb();
            PendingString* target = at == DW_AT_name ? &name : at == DW_AT_comp_dir ? &comp_dir : nullptr;
            switch (form) {
            case DW_FORM_string: {
                std::string s = c.cstr();
                if (target) target->value = std::move(s);
                break;
            }
            case DW_FORM_strp: {
                uint64_t off = c.fixed(offset_size);
                if (target && str.data) target->value = string_in(str, off);
                break;
            }
            case DW_FORM_line_strp: {
                uint64_t off = c.fixed(offset_size);
                if (target && line_str.data) target->value = string_in(line_str, off);
                break;
            }
            case DW_FORM_strx: case DW_FORM_strx1: case DW_FORM_strx2: case DW_FORM_strx3:
            case DW_FORM_strx4: {
                uint64_t idx = form == DW_FORM_strx  ? c.uleb()
                               : form == DW_FORM_strx1 ? c.fixed(1)
                               : form == DW_FORM_strx2 ? c.fixed(2)
                               : form == DW_FORM_strx3 ? c.fixed(3)
                                                       : c.fixed(4);
                if (target) target->strx = idx;
                break;
            }
            case DW_FORM_sec_offset: {
                uint64_t v = c.fixed(offset_size);
                if (at == DW_AT_str_offsets_base) str_offsets_base = v;
                break;
            }
            case DW_FORM_addr: c.skip(addr_size); break;
            case DW_FORM_data1: case DW_FORM_ref1: case DW_FORM_flag: case DW_FORM_addrx1: c.skip(1); break;
            case DW_FORM_data2: case DW_FORM_ref2: case DW_FORM_addrx2: c.skip(2); break;
            case DW_FORM_addrx3: c.skip(3); break;
            case DW_FORM_data4: case DW_FORM_ref4: case DW_FORM_ref_sup4: case DW_FORM_addrx4: c.skip(4); break;
            case DW_FORM_data8: case DW_FORM_ref8: case DW_FORM_ref_sig8: case DW_FORM_ref_sup8: c.skip(8); break;
            case DW_FORM_data16: c.skip(16); break;
            case DW_FORM_ref_addr: case DW_FORM_strp_sup: c.skip(uint64_t(offset_size)); break;
            case DW_FORM_udata: case DW_FORM_ref_udata: case DW_FORM_addrx: case DW_FORM_loclistx:
            case DW_FORM_rnglistx: c.uleb(); break;
            case DW_FORM_sdata: c.sleb(); break;
            case DW_FORM_flag_present: case DW_FORM_implicit_const: break;
            case DW_FORM_block1: c.skip(c.fixed(1)); break;
            case DW_FORM_block2: c.skip(c.fixed(2)); break;
            case DW_FORM_block4: c.skip(c.fixed(4)); break;
            case DW_FORM_block: case DW_FORM_exprloc: c.skip(c.uleb()); break;
            default: throw ElfError("unsupported DWARF form " + std::to_string(form));
            }
        }

        auto resolve = [&](const PendingString& p) -> std::string {
            if (p.value) return *p.value;
            if (p.strx && str_offsets.data && str.data) {
                Cursor o(str_offsets);
                o.seek(str_offsets_base + *p.strx * uint64_t(offset_size));
                return string_in(str, o.fixed(offset_size));
            }
            return {};
        };
        const std::string n = resolve(name);
        if (!n.empty()) {
            std::filesystem::path p(n);
            if (p.is_relative()) p = std::filesystem::path(resolve(comp_dir)) / p;
            units.push_back(p.lexically_normal());
        }
        c.seek(unit_end);
    }
    return units;
}

} // namespace hotpp::image
