#include "host/session_state.h"

#include "common/messages.h"

#include <fstream>
#include <sstream>

namespace hotpp::host {

using namespace hotpp::proto;

namespace {

constexpr uint32_t kStateMagic = 0x54535048; // "HPST"
constexpr uint16_t kStateVersion = 1;

void put_key(Writer& w, const link::SymbolKey& k) {
    w.u8(k.local ? 1 : 0);
    put(w, k.file);
    put(w, k.name);
}

link::SymbolKey get_key(Reader& r) {
    link::SymbolKey k;
    k.local = r.u8() != 0;
    k.file = get_string(r);
    k.name = get_string(r);
    return k;
}

void put_version(Writer& w, const link::FunctionVersion& v) {
    w.u64(v.entry);
    w.u8(v.site ? 1 : 0);
    if (v.site) {
        w.u8(v.site->entry_offset);
        w.u8(v.site->short_jump[0]);
        w.u8(v.site->short_jump[1]);
    }
}

link::FunctionVersion get_version(Reader& r) {
    link::FunctionVersion v;
    v.entry = r.u64();
    if (r.u8()) {
        link::SiteLayout s;
        s.entry_offset = r.u8();
        s.short_jump[0] = r.u8();
        s.short_jump[1] = r.u8();
        v.site = s;
    }
    return v;
}

} // namespace

bool save_session_state(const std::filesystem::path& path, const SessionState& st) {
    Writer w;
    w.u32(kStateMagic);
    w.u16(kStateVersion);
    w.u32(st.pid);
    w.u32(st.next_patch);
    w.u32(uint32_t(st.modules.size()));
    for (const SavedModule& m : st.modules) {
        w.u32(m.index);
        put(w, m.path);
        put_bytes(w, m.build_id);
        w.u64(m.region_base);
        w.u64(m.cursor);
    }
    w.u32(uint32_t(st.deltas.size()));
    for (const auto& [module, d] : st.deltas) {
        w.u32(module);
        w.u32(uint32_t(d.versions.size()));
        for (const auto& v : d.versions) {
            put_key(w, v.key);
            put_version(w, v.version);
        }
        w.u32(uint32_t(d.symbols.size()));
        for (const auto& s : d.symbols) {
            put_key(w, s.key);
            w.u8(uint8_t(s.entry.kind));
            w.u64(s.entry.address);
            w.u64(s.entry.size);
            w.u8(s.entry.writable ? 1 : 0);
            w.u32(uint32_t(s.entry.versions.size()));
            for (const auto& v : s.entry.versions) put_version(w, v);
        }
    }
    if (!w.ok()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::optional<SessionState> load_session_state(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    Reader r(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (r.u32() != kStateMagic || r.u16() != kStateVersion) return std::nullopt;
    SessionState st;
    st.pid = r.u32();
    st.next_patch = r.u32();
    const uint32_t modules = r.u32();
    if (!r.ok() || modules > r.remaining()) return std::nullopt;
    for (uint32_t i = 0; i < modules && r.ok(); ++i) {
        SavedModule m;
        m.index = r.u32();
        m.path = get_string(r);
        m.build_id = get_bytes(r);
        m.region_base = r.u64();
        m.cursor = r.u64();
        st.modules.push_back(std::move(m));
    }
    const uint32_t deltas = r.u32();
    if (!r.ok() || deltas > r.remaining()) return std::nullopt;
    for (uint32_t i = 0; i < deltas && r.ok(); ++i) {
        const uint32_t module = r.u32();
        link::IndexDelta d;
        const uint32_t versions = r.u32();
        if (!r.ok() || versions > r.remaining()) return std::nullopt;
        for (uint32_t k = 0; k < versions && r.ok(); ++k) {
            link::IndexDelta::NewVersion v;
            v.key = get_key(r);
            v.version = get_version(r);
            d.versions.push_back(std::move(v));
        }
        const uint32_t symbols = r.u32();
        if (!r.ok() || symbols > r.remaining()) return std::nullopt;
        for (uint32_t k = 0; k < symbols && r.ok(); ++k) {
            link::IndexDelta::NewSymbol s;
            s.key = get_key(r);
            s.entry.kind = link::SymbolKind(r.u8());
            s.entry.address = r.u64();
            s.entry.size = r.u64();
            s.entry.writable = r.u8() != 0;
            const uint32_t n = r.u32();
            if (!r.ok() || n > r.remaining()) return std::nullopt;
            for (uint32_t j = 0; j < n && r.ok(); ++j) s.entry.versions.push_back(get_version(r));
            d.symbols.push_back(std::move(s));
        }
        st.deltas.emplace_back(module, std::move(d));
    }
    if (!r.ok() || !r.at_end()) return std::nullopt;
    return st;
}

} // namespace hotpp::host
