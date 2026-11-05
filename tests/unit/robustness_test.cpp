// Malformed input: corrupted object files and executables must be rejected with an
// exception, never crash or read out of bounds (run under -DHOTPP_SANITIZE=ON to
// catch the latter). Deterministic; HOTPP_FUZZ_ITERATIONS scales it.
#include "host/elf/elf_file.h"
#include "host/image/dwarf_types.h"
#include "host/image/elf_image.h"
#include "host/link/linker.h"
#include "host/object/elf_object.h"
#include "unit/test.h"

#include <cstdlib>
#include <random>

using namespace hotpp;

namespace {

int iterations(int fallback) {
    const char* env = std::getenv("HOTPP_FUZZ_ITERATIONS");
    return env ? std::atoi(env) : fallback;
}

// Byte flips, biased toward the structures parsers trust: headers, section
// headers, and the start of sections (symbol tables, relocations, DWARF).
std::vector<uint8_t> mutate(const elf::ElfFile& f, std::mt19937_64& rng) {
    std::vector<uint8_t> b = f.bytes();
    auto pick = [&](uint64_t lo, uint64_t n) { return n ? lo + rng() % n : 0; };
    const int edits = 1 + int(rng() % 8);
    for (int e = 0; e < edits; ++e) {
        uint64_t at;
        switch (rng() % 4) {
        case 0: at = pick(0, sizeof(elf::Ehdr)); break;
        case 1: at = pick(f.header().shoff, f.sections().size() * sizeof(elf::Shdr)); break;
        case 2: {
            const auto& s = f.sections()[rng() % f.sections().size()];
            at = pick(s.offset, std::min<uint64_t>(s.size, 256));
            break;
        }
        default: at = pick(0, b.size()); break;
        }
        if (at >= b.size()) continue;
        switch (rng() % 3) {
        case 0: b[at] ^= uint8_t(1u << (rng() % 8)); break;
        case 1: b[at] = uint8_t(rng()); break;
        default: b[at] = rng() % 2 ? 0xff : 0x00; break;
        }
    }
    if (rng() % 16 == 0) b.resize(rng() % b.size()); // truncation
    return b;
}

} // namespace

HOTPP_TEST(corrupted_objects_are_rejected_not_crashed_on) {
    const elf::ElfFile original = elf::ElfFile::load(HOTPP_TEST_LINK_OBJECT);
    std::mt19937_64 rng(42);
    link::SymbolIndex index;
    link::LinkOptions opt;
    opt.region_cursor = 0x20000000;
    opt.region_end = 0x20000000 + (64 << 20);
    const auto resolve = [](const std::vector<std::string>& n) { return std::vector<uint64_t>(n.size(), 0x7f0000001000); };
    int parsed = 0, linked = 0;
    for (int i = 0, n = iterations(3000); i < n; ++i) {
        try {
            auto obj = object::ElfObject::from_file(elf::ElfFile::from_bytes(mutate(original, rng), "mutated.o"));
            ++parsed;
            link::link_patch({obj.get()}, index, resolve, opt);
            ++linked;
        } catch (const std::exception&) {
            // rejected: fine
        }
    }
    std::printf("    %d parsed, %d linked\n", parsed, linked);
    CHECK(parsed > 0); // the mutations didn't make every file unreadable
}

HOTPP_TEST(corrupted_executables_are_rejected_not_crashed_on) {
    const elf::ElfFile original = elf::ElfFile::load("/proc/self/exe");
    std::mt19937_64 rng(7);
    int parsed = 0;
    for (int i = 0, n = iterations(3000) / 10; i < n; ++i) {
        const std::vector<uint8_t> bytes = mutate(original, rng);
        try {
            auto img = image::ElfImage::from_file(elf::ElfFile::from_bytes(bytes, "mutated"));
            link::SymbolIndex index;
            index.add_image(*img, 0x555500000000);
            ++parsed;
        } catch (const std::exception&) {
        }
        if (i % 8 != 0) continue; // a full DWARF walk of this binary is slow under sanitizers
        try {
            image::dwarf_type_layouts(elf::ElfFile::from_bytes(bytes, "mutated"));
        } catch (const std::exception&) {
        }
    }
    std::printf("    %d parsed\n", parsed);
    CHECK(parsed > 0);
}
