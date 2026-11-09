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

HOTPP_TEST(msvc_dependency_scan_arguments) {
    CompileCommand cmd;
    cmd.arguments = {"cl.exe", "/O2", "/FoC:\\b\\x.obj", "/c", "C:\\s\\x.cpp"};
    const auto a = dependency_scan_arguments(cmd, "C:\\cache\\x.d");
    CHECK(std::find(a.begin(), a.end(), "/c") == a.end());
    CHECK(std::find(a.begin(), a.end(), "/Zs") != a.end());
    CHECK(std::find(a.begin(), a.end(), "/sourceDependencies") != a.end());
    CHECK(std::none_of(a.begin(), a.end(), [](const std::string& s) { return s.rfind("/Fo", 0) == 0; }));

    cmd.arguments = {"clang-cl", "/c", "--", "/s/x.cpp"};
    const auto c = dependency_scan_arguments(cmd, "/cache/x.d");
    CHECK(std::find(c.begin(), c.end(), "/clang:-MM") != c.end());
    CHECK(std::find(c.begin(), c.end(), "/clang:-MF/cache/x.d") != c.end());
    CHECK(c.back() == "/s/x.cpp");
}

HOTPP_TEST(source_dependencies_json_drops_system_headers) {
    const std::string json = R"({"Version":"1.1","Data":{"Source":"/s/x.cpp","ProvidedModule":"",
        "Includes":["/s/x.h","/sdk/include/windows.h","/s/sub/y.h"],"ImportedModules":[],"ImportedHeaderUnits":[]}})";
    const auto deps = parse_source_dependencies(json, {"/sdk/include"});
    CHECK(deps.size() == 3);
    CHECK(std::find(deps.begin(), deps.end(), std::filesystem::path("/s/x.h")) != deps.end());
    CHECK(std::find(deps.begin(), deps.end(), std::filesystem::path("/s/sub/y.h")) != deps.end());
    CHECK(std::find(deps.begin(), deps.end(), std::filesystem::path("/sdk/include/windows.h")) == deps.end());
    CHECK(parse_source_dependencies("not json", {}).empty());
}
