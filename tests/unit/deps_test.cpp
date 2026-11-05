#include "host/deps.h"
#include "unit/test.h"

#include <algorithm>

using namespace hotpp;

HOTPP_TEST(depfile_parses_continuations_and_escapes) {
    const auto deps = parse_depfile("a.o: /src/a.cpp /src/a.h \\\n  inc/b.h /src/with\\ space.h \\\n /src/$$x.h\n",
                                    "/build");
    CHECK(deps.size() == 5);
    CHECK(deps.size() == 5 && deps[0] == "/src/a.cpp");
    CHECK(deps.size() == 5 && deps[2] == "/build/inc/b.h");
    CHECK(deps.size() == 5 && deps[3] == "/src/with space.h");
    CHECK(deps.size() == 5 && deps[4] == "/src/$x.h");
}

HOTPP_TEST(depfile_stops_at_phony_targets) {
    const auto deps = parse_depfile("a.o: /s/a.cpp /s/a.h\n/s/a.h:\n", "/");
    CHECK(deps.size() == 2);
}

HOTPP_TEST(dependency_scan_arguments_strip_output) {
    CompileCommand cmd;
    cmd.arguments = {"c++", "-O2", "-MD", "-MF", "x.d", "-o", "x.o", "-c", "x.cpp"};
    const auto a = dependency_scan_arguments(cmd, "/tmp/x.d");
    CHECK(std::find(a.begin(), a.end(), "-c") == a.end());
    CHECK(std::find(a.begin(), a.end(), "x.o") == a.end());
    CHECK(std::find(a.begin(), a.end(), "-MD") == a.end());
    CHECK(std::find(a.begin(), a.end(), "-MM") != a.end());
    CHECK(a.back() == "/tmp/x.d");
}

HOTPP_TEST(dependency_graph_tracks_includers) {
    DependencyGraph g;
    g.set("/s/a.cpp", {"/s/a.cpp", "/s/common.h", "/s/a.h"});
    g.set("/s/b.cpp", {"/s/b.cpp", "/s/common.h"});
    CHECK(g.includers("/s/common.h").size() == 2);
    CHECK(g.includers("/s/a.h").size() == 1);
    CHECK(g.includers("/s/a.cpp").empty());
    g.set("/s/a.cpp", {"/s/a.cpp", "/s/a.h"}); // a.cpp no longer includes common.h
    CHECK(g.includers("/s/common.h").size() == 1);
    CHECK(g.headers().size() == 2);
}
