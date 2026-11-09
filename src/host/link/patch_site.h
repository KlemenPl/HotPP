// Recognises the padding -fpatchable-function-entry=7,5 (or /hotpatch +
// /FUNCTIONPADMIN) leaves around a function entry, and encodes the redirect.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace hotpp::link {

constexpr size_t kPadBytes = 5;          // NOPs before the entry, for E9 rel32
constexpr size_t kSiteProbeBytes = 5 + 6; // padding + optional endbr64 + 2-byte NOP

struct SiteLayout {
    uint8_t entry_offset = 0; // where the 2-byte short jump goes, from the entry (0, or 4 after endbr64)
    uint8_t short_jump[2] = {0xEB, 0};
};

// PatchableEntry: -fpatchable-function-entry=7,5 (NOPs before and at the entry).
// Hotpatch: /hotpatch + /FUNCTIONPADMIN (int3 or NOP padding before the entry,
// whose first instruction the compiler makes at least 2 bytes long).
enum class SiteStyle { PatchableEntry, Hotpatch };

// `bytes` starts kPadBytes before the function entry and holds `len` bytes.
// Returns the layout if the function can be redirected.
std::optional<SiteLayout> analyze_patch_site(const uint8_t* bytes, size_t len,
                                             SiteStyle style = SiteStyle::PatchableEntry);

} // namespace hotpp::link
