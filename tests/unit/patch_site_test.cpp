#include "host/link/patch_site.h"
#include "unit/test.h"

using namespace hotpp::link;

namespace {
std::optional<SiteLayout> analyze(const char* bytes, size_t n) {
    return analyze_patch_site(reinterpret_cast<const uint8_t*>(bytes), n);
}
} // namespace

HOTPP_TEST(patch_site_gcc_padding) {
    // 5 x nop, then nop nop, push %rbp
    const auto s = analyze("\x90\x90\x90\x90\x90\x90\x90\x55\x48\x89\xe5", 11);
    CHECK(s.has_value());
    CHECK(s && s->entry_offset == 0);
    CHECK(s && s->short_jump[0] == 0xEB && s->short_jump[1] == 0xF9); // jmp -7
}

HOTPP_TEST(patch_site_clang_padding) {
    // 5-byte multi-byte nop, then xchg %ax,%ax
    const auto s = analyze("\x0f\x1f\x44\x00\x00\x66\x90\x55\x48\x89\xe5", 11);
    CHECK(s.has_value());
    CHECK(s && s->short_jump[1] == 0xF9);
    CHECK(analyze("\x90\x90\x90\x90\x90\x66\x90\x55\x48\x89\xe5", 11).has_value());
}

HOTPP_TEST(patch_site_after_endbr64) {
    const auto s = analyze("\x90\x90\x90\x90\x90\xf3\x0f\x1e\xfa\x90\x90", 11);
    CHECK(s.has_value());
    CHECK(s && s->entry_offset == 4);
    CHECK(s && s->short_jump[1] == 0xF5); // jmp -11
}

HOTPP_TEST(patch_site_rejects_unpadded_functions) {
    CHECK(!analyze("\xc3\x90\x90\x90\x90\x90\x90\x55\x48\x89\xe5", 11)); // ret in the padding
    CHECK(!analyze("\x90\x90\x90\x90\x90\x55\x48\x89\xe5\x90\x90", 11)); // no entry nop
    CHECK(!analyze("\x90\x90\x90\x90\x90\x90", 6));                     // too short
}
