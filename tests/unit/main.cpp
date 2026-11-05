#include "unit/test.h"

int main() {
    int failed_cases = 0;
    for (const auto& c : hotpp::test::registry()) {
        const int before = hotpp::test::failures();
        c.fn();
        const bool ok = hotpp::test::failures() == before;
        failed_cases += !ok;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("%zu tests, %d failed\n", hotpp::test::registry().size(), failed_cases);
    return failed_cases ? 1 : 0;
}
