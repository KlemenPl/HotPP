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

inline void put(Writer& w, const std::string& s) { w.str(s.data(), s.size()); }
inline std::string get_string(Reader& r) {
    StrView s = r.str();
    return std::string(s.data, s.size);
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
    }
    w.u32(uint32_t(m.regions.size()));
    for (const RegionRecord& reg : m.regions) {
        w.u64(reg.base);
        w.u64(reg.size);
        w.u32(reg.module_index);
    }
}

inline bool decode(Reader& r, Hello& m) {
    constexpr size_t kModuleMin = 8 + 8 + 8 + 4;
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

} // namespace hotpp::proto
