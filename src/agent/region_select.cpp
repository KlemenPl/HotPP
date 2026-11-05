#include "agent/region_select.h"

#include <algorithm>

namespace hotpp::agent {

namespace {
uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
} // namespace

std::vector<uint64_t> near_region_candidates(const std::vector<AddressRange>& free_ranges,
                                             uint64_t module_start, uint64_t module_end,
                                             uint64_t size, uint64_t alignment,
                                             uint64_t above_guard) {
    std::vector<uint64_t> below, above;
    if (size == 0 || module_end <= module_start || module_end - module_start + size > kRel32Reach)
        return below;

    // The region start must be within reach of the module end, and the region end
    // within reach of the module start.
    const uint64_t lo_bound = module_end > kRel32Reach ? module_end - kRel32Reach : 0;
    const uint64_t hi_bound = module_start + kRel32Reach;

    for (const AddressRange& gap : free_ranges) {
        const uint64_t gs = std::max(gap.start, lo_bound);
        const uint64_t ge = std::min(gap.end, hi_bound);
        if (ge <= gs || ge - gs < size) continue;

        if (ge <= module_start) {
            const uint64_t r = align_down(ge - size, alignment);
            if (r >= gs) below.push_back(r);
        } else if (gs >= module_end) {
            uint64_t r = align_up(gs, alignment);
            if (r < gs || r + size > ge) continue;
            if (above_guard && r + above_guard + size <= ge) r += above_guard;
            above.push_back(r);
        }
        // Gaps inside the module's own span (between its segments) are skipped.
    }

    std::sort(below.begin(), below.end(), [](uint64_t a, uint64_t b) { return a > b; });
    std::sort(above.begin(), above.end());
    below.insert(below.end(), above.begin(), above.end());
    return below;
}

} // namespace hotpp::agent
