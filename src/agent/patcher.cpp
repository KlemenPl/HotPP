#include "agent/patcher.h"

#include "agent/platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hotpp::agent {

namespace {

using namespace hotpp::proto;

bool inside(uint64_t addr, uint64_t size, uint64_t base, uint64_t span) {
    return addr >= base && size <= span && addr - base <= span - size;
}

bool in_regions(uint64_t addr, uint64_t size, const std::vector<RegionRecord>& regions) {
    for (const RegionRecord& r : regions)
        if (inside(addr, size, r.base, r.size)) return true;
    return false;
}

// Bytes a patchable entry may hold: a short jump written by an earlier patch, or
// - with -fpatchable-function-entry, the 2-byte NOP the compiler emitted
// - with /hotpatch (Windows), any instruction, with int3 or NOP padding before it
bool patchable_entry(const uint8_t* p, const uint8_t* pad) {
    if (p[0] == 0xEB) return true;
    if (kPlatform == Platform::Windows) {
        for (int i = 0; i < 5; ++i)
            if (pad[i] != 0xCC && pad[i] != 0x90) return false;
        return true;
    }
    return (p[0] == 0x90 && p[1] == 0x90) || (p[0] == 0x66 && p[1] == 0x90);
}

std::string at(const char* what, uint64_t addr) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s at 0x%llx", what, static_cast<unsigned long long>(addr));
    return buf;
}

void store_u16(uint64_t addr, uint16_t value) {
#if defined(__GNUC__)
    __atomic_store_n(reinterpret_cast<uint16_t*>(addr), value, __ATOMIC_RELEASE);
#else
    *reinterpret_cast<volatile uint16_t*>(addr) = value;
#endif
}

struct SiteRange {
    uint64_t lo, hi; // bytes the site rewrites
};

SiteRange range_of(const PatchSite& s) {
    return {std::min(s.pad_addr, s.entry_addr), std::max(s.pad_addr + 5, s.entry_addr + 2)};
}

} // namespace

std::string apply_patch(const Patch& patch, const std::vector<RegionRecord>& regions,
                        const std::vector<ModuleRecord>& modules) {
    auto in_modules = [&](uint64_t addr, uint64_t size) {
        for (const ModuleRecord& m : modules)
            if (inside(addr, size, m.start, m.size)) return true;
        return false;
    };
    const uint64_t page = page_size();

    for (const Segment& seg : patch.segments) {
        if (seg.addr % page || seg.size % page || seg.bytes.size() > seg.size ||
            !in_regions(seg.addr, seg.size, regions))
            return at("segment outside the patch region", seg.addr);
    }
    for (const PatchSite& site : patch.sites) {
        const SiteRange r = range_of(site);
        if (!in_modules(r.lo, r.hi - r.lo) && !in_regions(r.lo, r.hi - r.lo, regions))
            return at("patch site outside patchable code", site.entry_addr);
        const int64_t rel = int64_t(site.target - (site.pad_addr + 5));
        if (rel != int64_t(int32_t(rel))) return at("patch target out of rel32 range", site.target);
        if (!patchable_entry(reinterpret_cast<const uint8_t*>(site.entry_addr), reinterpret_cast<const uint8_t*>(site.pad_addr)))
            return at("unexpected bytes at function entry", site.entry_addr);
    }

    // 1. New code and data. Nothing references it until the sites below are written.
    for (const Segment& seg : patch.segments) {
        if (!protect(seg.addr, seg.size, kProtRead | kProtWrite))
            return at("cannot make segment writable", seg.addr);
        auto* dst = reinterpret_cast<uint8_t*>(seg.addr);
        if (!seg.bytes.empty()) std::memcpy(dst, seg.bytes.data(), seg.bytes.size());
        std::memset(dst + seg.bytes.size(), 0, seg.size - seg.bytes.size());
    }
    for (const Segment& seg : patch.segments) {
        if (!protect(seg.addr, seg.size, seg.prot)) return at("cannot protect segment", seg.addr);
        if (seg.prot & kProtExec) flush_icache(seg.addr, seg.size);
    }

    // 2. Unwind tables, so exceptions can pass through the new code. On Windows they
    //    come as (table, entry count) pairs, relative to the region's module.
    if (kPlatform == Platform::Windows) {
        for (size_t i = 0; i + 1 < patch.eh_frames.size(); i += 2) {
            const uint64_t table = patch.eh_frames[i], count = patch.eh_frames[i + 1];
            const RegionRecord* region = nullptr;
            for (const RegionRecord& r : regions)
                if (inside(table, count * 12, r.base, r.size)) region = &r;
            if (!region || region->module_index >= modules.size()) return at("unwind table outside the patch region", table);
            if (!register_function_table(table, count, modules[region->module_index].base))
                log("warning: RtlAddFunctionTable failed; exceptions thrown through patched code will terminate");
        }
    } else {
        for (uint64_t frame : patch.eh_frames) {
            if (!in_regions(frame, 4, regions)) return at("unwind table outside the patch region", frame);
            if (!register_unwind_info(frame))
                log("warning: no __register_frame in this process; exceptions thrown through patched code will terminate");
        }
    }

    // 3. Dynamic initializers of new globals. They only see the patch's own code and
    //    data, so they run before anything can reach the new code.
    for (uint64_t init : patch.initializers) {
        if (!in_regions(init, 1, regions)) return at("initializer outside the patch region", init);
        try {
            reinterpret_cast<void (*)()>(init)();
        } catch (...) {
            return at("dynamic initializer threw", init);
        }
    }

    // 4. Redirect old functions with every other thread stopped outside the bytes
    //    being rewritten: long jump into the padding first, then the 2-byte short jump
    //    at the entry in one store. Code pages are made RWX rather than RW, because
    //    the agent's own code may share a page with a site.
    if (patch.sites.empty()) {
        for (const auto& object : patch.debug_objects) register_debug_object(object);
        return {};
    }
    std::vector<uint64_t> stopped_at;
    std::string error;
    for (int attempt = 0;; ++attempt) {
        if (!pause_other_threads(stopped_at, error)) return "cannot pause threads: " + error;
        bool clear = true;
        for (uint64_t rip : stopped_at)
            for (const PatchSite& site : patch.sites) {
                const SiteRange r = range_of(site);
                clear = clear && !(rip >= r.lo && rip < r.hi);
            }
        if (clear) break;
        resume_other_threads();
        if (attempt == 200) return "a thread kept stopping inside a function entry being patched";
        sleep_ms(1);
    }
    for (const PatchSite& site : patch.sites) {
        const SiteRange r = range_of(site);
        if (!protect(r.lo, r.hi - r.lo, kProtRead | kProtWrite | kProtExec)) {
            resume_other_threads();
            return at("cannot make code writable", site.entry_addr);
        }
        uint8_t jmp[5] = {0xE9};
        const int32_t rel = int32_t(site.target - (site.pad_addr + 5));
        std::memcpy(jmp + 1, &rel, 4);
        std::memcpy(reinterpret_cast<void*>(site.pad_addr), jmp, sizeof jmp);
        uint16_t entry;
        std::memcpy(&entry, site.entry_bytes, 2);
        store_u16(site.entry_addr, entry);
        protect(r.lo, r.hi - r.lo, kProtRead | kProtExec);
        flush_icache(r.lo, r.hi - r.lo);
    }
    sync_cores();
    resume_other_threads();
    for (const auto& object : patch.debug_objects) register_debug_object(object);
    return {};
}

} // namespace hotpp::agent
