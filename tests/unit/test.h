// Minimal self-registering test framework.
#pragma once

#include <cstdio>
#include <vector>

namespace hotpp::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace hotpp::test

#define HOTPP_TEST(name)                                                    \
    static void name();                                                     \
    static ::hotpp::test::Registrar name##_registrar(#name, name);          \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++::hotpp::test::failures();                                              \
        }                                                                             \
    } while (0)
