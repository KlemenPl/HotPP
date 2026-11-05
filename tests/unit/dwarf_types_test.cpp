#include "host/image/dwarf_types.h"
#include "unit/test.h"

#include <cstdio>

using namespace hotpp;

struct LayoutProbe {
    int a;
    double b;
    struct Inner {
        char c;
    } inner;
};

namespace hotpp_test {
struct Base {
    long base_field;
};
struct Derived : Base {
    int own;
};
} // namespace hotpp_test

// Keep the types in this binary's debug info.
volatile size_t layout_probe_size = sizeof(LayoutProbe) + sizeof(hotpp_test::Derived);
LayoutProbe layout_probe;
hotpp_test::Derived derived_probe;

HOTPP_TEST(dwarf_type_layouts_of_own_binary) {
    const auto layouts = image::dwarf_type_layouts(elf::ElfFile::load("/proc/self/exe"));
    if (layouts.empty()) {
        std::printf("    (no DWARF in this build; skipped)\n");
        return;
    }
    auto probe = layouts.find("LayoutProbe");
    CHECK(probe != layouts.end());
    if (probe != layouts.end()) {
        CHECK(probe->second.size == sizeof(LayoutProbe));
        CHECK(probe->second.members.size() == 3);
        CHECK(probe->second.members.size() == 3 && probe->second.members[1] == std::make_pair(std::string("b"), int64_t(8)));
    }
    auto inner = layouts.find("LayoutProbe::Inner");
    CHECK(inner != layouts.end() && inner->second.size == 1);
    auto derived = layouts.find("hotpp_test::Derived");
    CHECK(derived != layouts.end());
    if (derived != layouts.end()) {
        CHECK(derived->second.size == sizeof(hotpp_test::Derived));
        CHECK(derived->second.members.size() == 2);
        CHECK(!derived->second.members.empty() && derived->second.members[0].first == "<base 0>");
    }
}
