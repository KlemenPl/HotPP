// HotPP protocol messages. One encode/decode pair per message keeps the agent and
// the host in lockstep; bump proto::kVersion whenever a layout here changes.
#pragma once

#include "common/protocol.h"

#include <string>
#include <vector>

namespace hotpp::proto {

struct ModuleRecord {
    uint64_t base = 0;  // load bias (ELF) / image base (PE); symbol address = base + value
    uint64_t start = 0; // lowest mapped address
    uint64_t size = 0;  // mapped span in bytes, page aligned
    std::string path;
    std::vector<uint8_t> build_id; // GNU build ID of the loaded image; empty if unknown
};

struct RegionRecord {
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t module_index = 0; // module the region sits within rel32 reach of
};

struct Hello {
    uint32_t pid = 0;
    Platform platform = Platform::Unknown;
    Arch arch = Arch::Unknown;
    std::string exe;
    std::string compile_commands;
    std::vector<ModuleRecord> modules; // [0] is the main executable
    std::vector<RegionRecord> regions;
};

struct HelloAck {
    uint32_t host_pid = 0;
    std::string host_version;
};

struct LogMsg {
    LogLevel level = LogLevel::Info;
    std::string text;
};

// A page-aligned range in a patch region. `bytes` may be shorter than `size`; the
// rest is zero-filled (bss).
struct Segment {
    uint64_t addr = 0;
    uint64_t size = 0;
    uint8_t prot = 0; // kProt* bits
    std::vector<uint8_t> bytes;
};

// Redirects one function entry: `E9 rel32` to `target` goes into the 5 padding bytes
// at `pad_addr`, then `entry_bytes` (a short jump back into the padding) are stored
// atomically at `entry_addr`.
struct PatchSite {
    uint64_t pad_addr = 0;
    uint64_t entry_addr = 0;
    uint64_t target = 0;
    uint8_t entry_bytes[2] = {0, 0};
};

struct Patch {
    uint32_t id = 0;
    std::vector<Segment> segments;
    std::vector<PatchSite> sites;
    uint64_t eh_frame = 0;              // start of the patch's .eh_frame (zero-terminated), or 0
    std::vector<uint64_t> initializers; // dynamic initializers to call, in order, before redirecting
};

struct PatchResult {
    uint32_t id = 0;
    bool ok = false;
    std::string error;
};

struct ResolveRequest {
    uint32_t id = 0;
    std::vector<std::string> names;
};

struct ResolveReply {
    uint32_t id = 0;
    std::vector<uint64_t> addresses; // 0 where a name was not found
};

inline void put(Writer& w, const std::string& s) { w.str(s.data(), s.size()); }
inline std::string get_string(Reader& r) {
    StrView s = r.str();
    return std::string(s.data, s.size);
}
inline void put_bytes(Writer& w, const std::vector<uint8_t>& b) {
    w.u32(uint32_t(b.size()));
    w.raw(b.data(), b.size());
}
inline std::vector<uint8_t> get_bytes(Reader& r) {
    uint32_t n = r.u32();
    const uint8_t* p = r.raw(n);
    return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
}

inline void encode(Writer& w, const Hello& m) {
    w.u32(m.pid);
    w.u8(uint8_t(m.platform));
    w.u8(uint8_t(m.arch));
    put(w, m.exe);
    put(w, m.compile_commands);
    w.u32(uint32_t(m.modules.size()));
    for (const ModuleRecord& mod : m.modules) {
        w.u64(mod.base);
        w.u64(mod.start);
        w.u64(mod.size);
        put(w, mod.path);
        put_bytes(w, mod.build_id);
    }
    w.u32(uint32_t(m.regions.size()));
    for (const RegionRecord& reg : m.regions) {
        w.u64(reg.base);
        w.u64(reg.size);
        w.u32(reg.module_index);
    }
}

inline bool decode(Reader& r, Hello& m) {
    constexpr size_t kModuleMin = 8 + 8 + 8 + 4 + 4;
    constexpr size_t kRegionMin = 8 + 8 + 4;
    m.pid = r.u32();
    m.platform = Platform(r.u8());
    m.arch = Arch(r.u8());
    m.exe = get_string(r);
    m.compile_commands = get_string(r);
    uint32_t modules = r.u32();
    if (!r.ok() || modules > r.remaining() / kModuleMin) return false;
    m.modules.resize(modules);
    for (ModuleRecord& mod : m.modules) {
        mod.base = r.u64();
        mod.start = r.u64();
        mod.size = r.u64();
        mod.path = get_string(r);
        mod.build_id = get_bytes(r);
    }
    uint32_t regions = r.u32();
    if (!r.ok() || regions > r.remaining() / kRegionMin) return false;
    m.regions.resize(regions);
    for (RegionRecord& reg : m.regions) {
        reg.base = r.u64();
        reg.size = r.u64();
        reg.module_index = r.u32();
    }
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const HelloAck& m) {
    w.u32(m.host_pid);
    put(w, m.host_version);
}

inline bool decode(Reader& r, HelloAck& m) {
    m.host_pid = r.u32();
    m.host_version = get_string(r);
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const LogMsg& m) {
    w.u8(uint8_t(m.level));
    put(w, m.text);
}

inline bool decode(Reader& r, LogMsg& m) {
    m.level = LogLevel(r.u8());
    m.text = get_string(r);
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const Patch& m) {
    w.u32(m.id);
    w.u32(uint32_t(m.segments.size()));
    for (const Segment& s : m.segments) {
        w.u64(s.addr);
        w.u64(s.size);
        w.u8(s.prot);
        put_bytes(w, s.bytes);
    }
    w.u32(uint32_t(m.sites.size()));
    for (const PatchSite& s : m.sites) {
        w.u64(s.pad_addr);
        w.u64(s.entry_addr);
        w.u64(s.target);
        w.u8(s.entry_bytes[0]);
        w.u8(s.entry_bytes[1]);
    }
    w.u64(m.eh_frame);
    w.u32(uint32_t(m.initializers.size()));
    for (uint64_t a : m.initializers) w.u64(a);
}

inline bool decode(Reader& r, Patch& m) {
    constexpr size_t kSegmentMin = 8 + 8 + 1 + 4;
    constexpr size_t kSiteSize = 8 + 8 + 8 + 2;
    m.id = r.u32();
    uint32_t segments = r.u32();
    if (!r.ok() || segments > r.remaining() / kSegmentMin) return false;
    m.segments.resize(segments);
    for (Segment& s : m.segments) {
        s.addr = r.u64();
        s.size = r.u64();
        s.prot = r.u8();
        s.bytes = get_bytes(r);
        if (s.bytes.size() > s.size) return false;
    }
    uint32_t sites = r.u32();
    if (!r.ok() || sites > r.remaining() / kSiteSize) return false;
    m.sites.resize(sites);
    for (PatchSite& s : m.sites) {
        s.pad_addr = r.u64();
        s.entry_addr = r.u64();
        s.target = r.u64();
        s.entry_bytes[0] = r.u8();
        s.entry_bytes[1] = r.u8();
    }
    m.eh_frame = r.u64();
    uint32_t inits = r.u32();
    if (!r.ok() || inits > r.remaining() / 8) return false;
    m.initializers.resize(inits);
    for (uint64_t& a : m.initializers) a = r.u64();
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const PatchResult& m) {
    w.u32(m.id);
    w.u8(m.ok ? 1 : 0);
    put(w, m.error);
}

inline bool decode(Reader& r, PatchResult& m) {
    m.id = r.u32();
    m.ok = r.u8() != 0;
    m.error = get_string(r);
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const ResolveRequest& m) {
    w.u32(m.id);
    w.u32(uint32_t(m.names.size()));
    for (const std::string& n : m.names) put(w, n);
}

inline bool decode(Reader& r, ResolveRequest& m) {
    m.id = r.u32();
    uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 4) return false;
    m.names.resize(n);
    for (std::string& s : m.names) s = get_string(r);
    return r.ok() && r.at_end();
}

inline void encode(Writer& w, const ResolveReply& m) {
    w.u32(m.id);
    w.u32(uint32_t(m.addresses.size()));
    for (uint64_t a : m.addresses) w.u64(a);
}

inline bool decode(Reader& r, ResolveReply& m) {
    m.id = r.u32();
    uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / 8) return false;
    m.addresses.resize(n);
    for (uint64_t& a : m.addresses) a = r.u64();
    return r.ok() && r.at_end();
}

} // namespace hotpp::proto
