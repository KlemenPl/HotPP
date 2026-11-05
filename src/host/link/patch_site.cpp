#include "host/link/patch_site.h"

#include <cstring>

namespace hotpp::link {

namespace {

// Length of the NOP instruction at p (one of the recommended Intel encodings), or 0.
size_t nop_length(const uint8_t* p, size_t avail) {
    static const uint8_t* const kNops[] = {
        reinterpret_cast<const uint8_t*>("\x90"),
        reinterpret_cast<const uint8_t*>("\x66\x90"),
        reinterpret_cast<const uint8_t*>("\x0f\x1f\x00"),
        reinterpret_cast<const uint8_t*>("\x0f\x1f\x40\x00"),
        reinterpret_cast<const uint8_t*>("\x0f\x1f\x44\x00\x00"),
    };
    static const size_t kLens[] = {1, 2, 3, 4, 5};
    for (size_t i = 0; i < 5; ++i)
        if (kLens[i] <= avail && std::memcmp(p, kNops[i], kLens[i]) == 0) return kLens[i];
    return 0;
}

} // namespace

std::optional<SiteLayout> analyze_patch_site(const uint8_t* bytes, size_t len) {
    if (len < kPadBytes + 2) return std::nullopt;
    // The padding must be NOPs exactly covering 5 bytes: never executed, safe to overwrite.
    for (size_t pos = 0; pos < kPadBytes;) {
        const size_t n = nop_length(bytes + pos, kPadBytes - pos);
        if (n == 0) return std::nullopt;
        pos += n;
    }
    const uint8_t* entry = bytes + kPadBytes;
    size_t avail = len - kPadBytes;
    SiteLayout layout;
    if (avail >= 6 && std::memcmp(entry, "\xf3\x0f\x1e\xfa", 4) == 0) { // endbr64
        layout.entry_offset = 4;
        entry += 4;
        avail -= 4;
    }
    // A single instruction of at least 2 bytes that the 2-byte store replaces whole.
    const bool two_nops = entry[0] == 0x90 && entry[1] == 0x90;
    const bool xchg_ax = entry[0] == 0x66 && entry[1] == 0x90;
    if (!two_nops && !xchg_ax) return std::nullopt;
    // Jump from the end of the short jump back to the start of the padding.
    const int disp = -int(kPadBytes + layout.entry_offset + 2);
    layout.short_jump[1] = uint8_t(int8_t(disp));
    return layout;
}

} // namespace hotpp::link
