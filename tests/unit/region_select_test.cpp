#include "agent/region_select.h"
#include "unit/test.h"

using namespace hotpp::agent;

namespace {

constexpr uint64_t MiB = 1ull << 20;
constexpr uint64_t GiB = 1ull << 30;
constexpr uint64_t kAlign = 64 * 1024;

uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

// Every byte of [r, r+size) within reach of every byte of [ms, me).
bool in_reach(uint64_t r, uint64_t size, uint64_t ms, uint64_t me) {
    return distance(r, me) <= kRel32Reach && distance(r + size, ms) <= kRel32Reach;
}

} // namespace

HOTPP_TEST(prefers_closest_gap_below_module) {
    const uint64_t ms = 0x555500000000, me = ms + 16 * MiB;
    const std::vector<AddressRange> free = {
        {0x10000, ms - 1 * GiB},          // far below
        {ms - 900 * MiB, ms - 1 * MiB},   // just below
        {me + 32 * MiB, 0x7FFFFFFFF000},  // above
    };
    auto c = near_region_candidates(free, ms, me, 512 * MiB, kAlign, 64 * MiB);
    CHECK(!c.empty());
    CHECK(c[0] == ((ms - 1 * MiB - 512 * MiB) & ~(kAlign - 1)));
    for (uint64_t r : c) CHECK(in_reach(r, 512 * MiB, ms, me));
    CHECK(c[0] % kAlign == 0);
}

HOTPP_TEST(falls_back_above_with_heap_guard) {
    // Non-PIE layout: almost nothing below the executable.
    const uint64_t ms = 0x400000, me = 0x600000;
    const std::vector<AddressRange> free = {{0x10000, ms}, {me, 0x7FFFFFFFF000}};
    auto c = near_region_candidates(free, ms, me, 512 * MiB, kAlign, 64 * MiB);
    CHECK(c.size() == 1);
    CHECK(c[0] == me + 64 * MiB);
    CHECK(in_reach(c[0], 512 * MiB, ms, me));
}

HOTPP_TEST(skips_gaps_out_of_reach_or_too_small) {
    const uint64_t ms = 0x555500000000, me = ms + 16 * MiB;
    const std::vector<AddressRange> free = {
        {0x10000, ms - 3 * GiB},          // out of reach
        {ms - 100 * MiB, ms - 1 * MiB},   // too small for 512 MiB
        {me + 3 * GiB, 0x7FFFFFFFF000},   // out of reach
    };
    CHECK(near_region_candidates(free, ms, me, 512 * MiB, kAlign, 0).empty());
}

HOTPP_TEST(clips_large_gap_to_reach) {
    // One huge gap below whose start is out of reach: the region hugs the module.
    const uint64_t ms = 0x555500000000, me = ms + 16 * MiB;
    auto c = near_region_candidates({{0x10000, ms}}, ms, me, 512 * MiB, kAlign, 0);
    CHECK(c.size() == 1);
    CHECK(c[0] == ms - 512 * MiB);
    CHECK(in_reach(c[0], 512 * MiB, ms, me));
}

HOTPP_TEST(guard_dropped_when_gap_is_tight) {
    const uint64_t ms = 0x400000, me = 0x600000;
    const std::vector<AddressRange> free = {{me, me + 520 * MiB}};
    auto c = near_region_candidates(free, ms, me, 512 * MiB, kAlign, 64 * MiB);
    CHECK(c.size() == 1);
    CHECK(c[0] == me);
}

HOTPP_TEST(no_candidates_when_module_plus_region_exceeds_reach) {
    const uint64_t ms = 0x10000000, me = ms + 1800 * MiB;
    const std::vector<AddressRange> free = {{0x10000, ms}, {me, 0x7FFFFFFFF000}};
    CHECK(near_region_candidates(free, ms, me, 512 * MiB, kAlign, 0).empty());
}
