// Picks where to reserve the patch region so every byte of it is within rel32
// reach (±2 GiB) of every byte of the module. Pure logic, unit tested.
#pragma once

#include <cstdint>
#include <vector>

namespace hotpp::agent {

struct AddressRange {
    uint64_t start = 0;
    uint64_t end = 0; // exclusive
};

// Slightly under 2 GiB, leaving room for instruction lengths and addends.
constexpr uint64_t kRel32Reach = 0x7FF00000ull;

// Candidate start addresses for a region of `size` bytes, best first: closest below
// the module, then closest above it. `free_ranges` must be unmapped address ranges.
// `alignment` must be a power of two. Above the module, `above_guard` bytes are left
// free when the gap allows it, so the brk heap that follows an executable can grow.
std::vector<uint64_t> near_region_candidates(const std::vector<AddressRange>& free_ranges,
                                             uint64_t module_start, uint64_t module_end,
                                             uint64_t size, uint64_t alignment,
                                             uint64_t above_guard);

} // namespace hotpp::agent
