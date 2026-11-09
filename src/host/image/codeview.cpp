#include "host/image/codeview.h"

#include <cstring>

namespace hotpp::codeview {

namespace {

struct Cursor {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok(size_t want) const { return pos <= n && want <= n - pos; }
    void need(size_t want) const {
        if (!ok(want)) throw CvError("truncated CodeView record");
    }
    uint8_t u8() {
        need(1);
        return p[pos++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v;
        std::memcpy(&v, p + pos, 2);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v;
        std::memcpy(&v, p + pos, 4);
        pos += 4;
        return v;
    }
    void skip(size_t k) {
        need(k);
        pos += k;
    }
    // A numeric leaf: the value directly, or a kind followed by it.
    int64_t numeric() {
        const uint16_t v = u16();
        if (v < 0x8000) return v;
        switch (v) {
        case 0x8000: return int8_t(u8());
        case 0x8001: return int16_t(u16());
        case 0x8002: return u16();
        case 0x8003: return int32_t(u32());
        case 0x8004: return u32();
        case 0x8009: case 0x800A: {
            const uint64_t lo = u32(), hi = u32();
            return int64_t(lo | (hi << 32));
        }
        default: throw CvError("unsupported numeric leaf");
        }
    }
    std::string cstr() {
        const size_t start = pos;
        while (pos < n && p[pos]) ++pos;
        if (pos >= n) throw CvError("unterminated CodeView string");
        std::string s(reinterpret_cast<const char*>(p + start), pos - start);
        ++pos;
        return s;
    }
    // Field-list entries are padded with LF_PAD bytes (0xF0 + count).
    void pad() {
        while (pos < n && p[pos] >= 0xF0) ++pos;
    }
};

// Size of a built-in type (index < 0x1000).
uint64_t simple_size(uint32_t ti) {
    if ((ti >> 8) & 0x7) return 8; // a pointer mode: 64-bit pointers
    switch (ti & 0xFF) {
    case 0x10: case 0x20: case 0x68: case 0x69: case 0x70: case 0x7C: case 0x30: return 1;
    case 0x11: case 0x21: case 0x72: case 0x73: case 0x71: case 0x7A: case 0x31: case 0x46: return 2;
    case 0x12: case 0x22: case 0x74: case 0x75: case 0x7B: case 0x40: case 0x32: case 0x08: return 4;
    case 0x13: case 0x23: case 0x76: case 0x77: case 0x41: case 0x33: return 8;
    case 0x42: return 10;
    case 0x14: case 0x24: case 0x78: case 0x79: case 0x43: return 16;
    default: return 0;
    }
}

bool is_udt(uint16_t k) {
    return k == LF_CLASS || k == LF_STRUCTURE || k == LF_UNION || k == LF_ENUM || k == LF_INTERFACE;
}

constexpr uint16_t kForwardRef = 0x80, kScoped = 0x100, kHasUniqueName = 0x200;

struct Udt {
    uint16_t property = 0;
    uint32_t fields = 0;
    uint64_t size = 0;
    std::string name, unique;
};

Udt read_udt(uint16_t kind, const uint8_t* p, size_t n) {
    Cursor c{p, n};
    Udt u;
    c.u16(); // member count
    u.property = c.u16();
    if (kind == LF_ENUM) {
        c.u32(); // underlying type
        u.fields = c.u32();
    } else {
        u.fields = c.u32();
        if (kind != LF_UNION) {
            c.u32(); // derived
            c.u32(); // vshape
        }
        u.size = uint64_t(c.numeric());
    }
    u.name = c.cstr();
    if ((u.property & kHasUniqueName) && c.ok(1)) u.unique = c.cstr();
    return u;
}

std::string hex(uint32_t v) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "%x", v);
    return buf;
}

std::string fp(const TypeTable& t, uint32_t ti, const TypeTable* ids, int depth) {
    if (ti < 0x1000) return "s" + hex(ti);
    if (depth > 12) return "~";
    if (!t.has(ti)) return "?" + hex(ti);
    size_t n = 0;
    const uint8_t* p = t.data(ti, n);
    const uint16_t kind = t.kind(ti);
    Cursor c{p, n};
    switch (kind) {
    case LF_MODIFIER: {
        const uint32_t base = c.u32();
        return "m" + hex(c.u16()) + "(" + fp(t, base, ids, depth + 1) + ")";
    }
    case LF_POINTER: {
        const uint32_t base = c.u32();
        const uint32_t attr = c.u32();
        // kind, mode and the const/volatile/ref bits; not the size, which may differ
        return "p" + hex(attr & 0x1FFF) + "(" + fp(t, base, ids, depth + 1) + ")";
    }
    case LF_PROCEDURE: {
        const uint32_t ret = c.u32();
        c.u8();
        c.u8();
        c.u16();
        const uint32_t args = c.u32();
        return "f(" + fp(t, ret, ids, depth + 1) + ";" + fp(t, args, ids, depth + 1) + ")";
    }
    case LF_MFUNCTION: {
        const uint32_t ret = c.u32(), cls = c.u32(), self = c.u32();
        c.u8();
        c.u8();
        c.u16();
        const uint32_t args = c.u32();
        return "mf(" + fp(t, cls, ids, depth + 1) + ";" + fp(t, ret, ids, depth + 1) + ";" +
               fp(t, self, ids, depth + 1) + ";" + fp(t, args, ids, depth + 1) + ")";
    }
    case LF_ARGLIST: {
        const uint32_t count = c.u32();
        std::string s = "a(";
        for (uint32_t i = 0; i < count && i < 256; ++i) s += fp(t, c.u32(), ids, depth + 1) + ",";
        return s + ")";
    }
    case LF_ARRAY: {
        const uint32_t elem = c.u32();
        c.u32();
        return "r" + std::to_string(c.numeric()) + "(" + fp(t, elem, ids, depth + 1) + ")";
    }
    case LF_BITFIELD: {
        const uint32_t base = c.u32();
        const uint8_t len = c.u8(), pos = c.u8();
        return "b" + std::to_string(len) + "." + std::to_string(pos) + "(" + fp(t, base, ids, depth + 1) + ")";
    }
    default:
        if (is_udt(kind)) {
            const Udt u = read_udt(kind, p, n);
            return "u" + hex(kind) + ":" + (u.unique.empty() ? u.name : u.unique);
        }
        return "k" + hex(kind);
    }
}

// The function type behind a procedure symbol's type index: a type (S_GPROC32), or
// an id (S_GPROC32_ID) naming one.
uint32_t function_type(uint16_t kind, uint32_t ti, const TypeTable* ids) {
    if (kind != S_LPROC32_ID && kind != S_GPROC32_ID && kind != S_LPROC32_DPC_ID) return ti;
    if (!ids || !ids->has(ti)) return ti;
    const uint16_t k = ids->kind(ti);
    if (k != LF_FUNC_ID && k != LF_MFUNC_ID) return ti;
    size_t n = 0;
    Cursor c{ids->data(ti, n), n};
    c.u32(); // scope or parent type
    return c.u32();
}

} // namespace

TypeTable TypeTable::parse(const uint8_t* data, size_t size, uint32_t first_index) {
    TypeTable t;
    t.first_ = first_index;
    size_t pos = 0;
    while (pos + 4 <= size) {
        uint16_t len, kind;
        std::memcpy(&len, data + pos, 2);
        std::memcpy(&kind, data + pos + 2, 2);
        if (len < 2 || pos + 2 + len > size) throw CvError("truncated type record");
        t.records_.push_back({data + pos + 4, kind, uint16_t(len - 2)});
        pos += 2 + size_t(len);
    }
    return t;
}

uint16_t TypeTable::kind(uint32_t index) const {
    if (!has(index)) throw CvError("bad type index " + hex(index));
    return records_[index - first_].kind;
}

const uint8_t* TypeTable::data(uint32_t index, size_t& size) const {
    if (!has(index)) throw CvError("bad type index " + hex(index));
    const Rec& r = records_[index - first_];
    size = r.size;
    return r.p;
}

std::string fingerprint(const TypeTable& types, uint32_t index, const TypeTable* ids) {
    return fp(types, index, ids, 0);
}

uint64_t type_size(const TypeTable& t, uint32_t ti) {
    for (int depth = 0; depth < 16; ++depth) {
        if (ti < 0x1000) return simple_size(ti);
        if (!t.has(ti)) return 0;
        size_t n = 0;
        const uint8_t* p = t.data(ti, n);
        const uint16_t kind = t.kind(ti);
        Cursor c{p, n};
        switch (kind) {
        case LF_MODIFIER: ti = c.u32(); continue;
        case LF_POINTER: {
            c.u32();
            const uint32_t attr = c.u32();
            const uint32_t size = (attr >> 13) & 0x3F;
            return size ? size : 8;
        }
        case LF_ARRAY: {
            c.u32();
            c.u32();
            return uint64_t(c.numeric());
        }
        case LF_ENUM: {
            c.u16();
            c.u16();
            ti = c.u32();
            continue;
        }
        default:
            if (is_udt(kind)) return read_udt(kind, p, n).size;
            return 0;
        }
    }
    return 0;
}

std::unordered_map<std::string, image::TypeLayout> type_layouts(const TypeTable& t) {
    std::unordered_map<std::string, image::TypeLayout> out;
    for (uint32_t ti = t.first(); t.has(ti); ++ti) {
        const uint16_t kind = t.kind(ti);
        if (kind != LF_CLASS && kind != LF_STRUCTURE && kind != LF_UNION && kind != LF_INTERFACE) continue;
        size_t n = 0;
        const uint8_t* rec = t.data(ti, n);
        Udt u;
        try {
            u = read_udt(kind, rec, n);
        } catch (const CvError&) {
            continue;
        }
        if ((u.property & (kForwardRef | kScoped)) || u.name.empty() || u.name.find("<unnamed") != std::string::npos ||
            u.name.find("<lambda") != std::string::npos || out.count(u.name))
            continue;
        image::TypeLayout layout;
        layout.size = u.size;
        int bases = 0;
        // The field list, following LF_INDEX continuations.
        for (uint32_t list = u.fields; list && t.has(list) && t.kind(list) == LF_FIELDLIST;) {
            size_t ln = 0;
            Cursor c{t.data(list, ln), ln};
            uint32_t next = 0;
            try {
                while (c.ok(2)) {
                    const uint16_t leaf = c.u16();
                    switch (leaf) {
                    case LF_MEMBER: {
                        c.u16();
                        c.u32();
                        const int64_t off = c.numeric();
                        layout.members.emplace_back(c.cstr(), off);
                        break;
                    }
                    case LF_BCLASS: {
                        c.u16();
                        c.u32();
                        layout.members.emplace_back("<base " + std::to_string(bases++) + ">", c.numeric());
                        break;
                    }
                    case LF_VBCLASS: case LF_IVBCLASS:
                        c.u16();
                        c.u32();
                        c.u32();
                        c.numeric();
                        c.numeric();
                        break;
                    case LF_INDEX:
                        c.u16();
                        next = c.u32();
                        break;
                    case LF_VFUNCTAB: c.u16(); c.u32(); break;
                    case LF_STMEMBER: c.u16(); c.u32(); c.cstr(); break;
                    case LF_METHOD: c.u16(); c.u32(); c.cstr(); break;
                    case LF_NESTTYPE: c.u16(); c.u32(); c.cstr(); break;
                    case LF_ENUMERATE: c.u16(); c.numeric(); c.cstr(); break;
                    case LF_ONEMETHOD: {
                        const uint16_t attr = c.u16();
                        c.u32();
                        const uint16_t mprop = (attr >> 2) & 7;
                        if (mprop == 4 || mprop == 6) c.u32(); // introducing virtual: vtable offset
                        c.cstr();
                        break;
                    }
                    default: throw CvError("unknown field leaf");
                    }
                    c.pad();
                }
            } catch (const CvError&) {
                break; // keep what was read
            }
            list = next;
        }
        out.emplace(u.name, std::move(layout));
    }
    return out;
}

void collect_symbols(const uint8_t* records, size_t size, size_t base, const TypeTable& types, const TypeTable* ids,
                     std::vector<NamedSymbol>& out) {
    std::vector<std::string> scopes; // keys of enclosing procedures ("" for other scopes)
    size_t pos = 0;
    while (pos + 4 <= size) {
        uint16_t len, kind;
        std::memcpy(&len, records + pos, 2);
        std::memcpy(&kind, records + pos + 2, 2);
        if (len < 2 || pos + 2 + len > size) throw CvError("truncated symbol record");
        const uint8_t* p = records + pos + 4;
        const size_t n = size_t(len) - 2;
        Cursor c{p, n};
        switch (kind) {
        case S_LPROC32: case S_GPROC32: case S_LPROC32_ID: case S_GPROC32_ID: case S_LPROC32_DPC: case S_LPROC32_DPC_ID: {
            c.skip(12); // parent, end, next
            NamedSymbol s;
            s.kind = kind;
            s.procedure = true;
            s.local = kind != S_GPROC32 && kind != S_GPROC32_ID;
            s.length = c.u32();
            c.skip(8); // debug start, end
            s.type = function_type(kind, c.u32(), ids);
            s.offset_field = base + pos + 4 + c.pos;
            s.offset = c.u32();
            s.segment = c.u16();
            c.u8(); // flags
            s.name = c.cstr();
            std::string scope;
            for (const std::string& e : scopes)
                if (!e.empty()) scope = e + "::";
            s.key = std::string(1, kBridgeMark) + scope + s.name + "(" + fingerprint(types, s.type, ids) + ")";
            scopes.push_back(s.key.substr(1));
            out.push_back(std::move(s));
            break;
        }
        case S_LDATA32: case S_GDATA32: case S_LTHREAD32: case S_GTHREAD32: {
            NamedSymbol s;
            s.kind = kind;
            s.local = kind == S_LDATA32 || kind == S_LTHREAD32;
            s.tls = kind == S_LTHREAD32 || kind == S_GTHREAD32;
            s.type = c.u32();
            s.offset_field = base + pos + 4 + c.pos;
            s.offset = c.u32();
            s.segment = c.u16();
            s.name = c.cstr();
            std::string scope;
            for (const std::string& e : scopes)
                if (!e.empty()) scope = e + "::";
            s.key = std::string(1, kBridgeMark) + scope + s.name + ":" + fingerprint(types, s.type, ids);
            out.push_back(std::move(s));
            break;
        }
        case S_BLOCK32: case S_THUNK32: case S_SEPCODE: case S_INLINESITE:
            scopes.emplace_back(); // a scope that isn't a function
            break;
        case S_END: case S_PROC_ID_END: case S_INLINESITE_END:
            if (!scopes.empty()) scopes.pop_back();
            break;
        default: break;
        }
        pos += 2 + size_t(len);
    }
}

bool is_bridge_key(const std::string& name) { return !name.empty() && name[0] == kBridgeMark; }

std::string bridge_display(const std::string& key) {
    if (!is_bridge_key(key)) return key;
    // Drop the type fingerprints: "(f(...))" after procedures, ":..." after data.
    std::string out;
    int depth = 0;
    for (size_t i = 1; i < key.size(); ++i) {
        const char ch = key[i];
        if (ch == '(') {
            if (depth++ == 0) out += "()";
            continue;
        }
        if (ch == ')') {
            --depth;
            continue;
        }
        if (depth) continue;
        if (ch == ':' && (i + 1 >= key.size() || key[i + 1] != ':') && (i == 0 || key[i - 1] != ':')) break;
        out += ch;
    }
    return out;
}

BuildInfo build_info(const TypeTable& ids, uint32_t index) {
    BuildInfo b;
    if (!ids.has(index) || ids.kind(index) != LF_BUILDINFO) return b;
    size_t n = 0;
    Cursor c{ids.data(index, n), n};
    const uint16_t count = c.u16();
    auto str = [&](uint32_t id) -> std::string {
        if (!ids.has(id) || ids.kind(id) != LF_STRING_ID) return {};
        size_t sn = 0;
        Cursor s{ids.data(id, sn), sn};
        s.u32(); // substring list
        return s.cstr();
    };
    std::vector<uint32_t> args;
    for (uint16_t i = 0; i < count && c.ok(4); ++i) args.push_back(c.u32());
    if (args.size() > 0) b.directory = str(args[0]);
    if (args.size() > 2) b.source = str(args[2]);
    return b;
}

} // namespace hotpp::codeview
