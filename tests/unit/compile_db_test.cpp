#include "host/compile_db.h"
#include "unit/test.h"

#include <algorithm>

using namespace hotpp;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

HOTPP_TEST(split_command_line_quoting) {
    const auto a = split_command_line(R"(/usr/bin/c++  -DNAME="a b" '-DX=it'"'"'s' -I/p\ q -c x.cpp)");
    CHECK(a.size() == 6);
    CHECK(a[0] == "/usr/bin/c++");
    CHECK(a[1] == "-DNAME=a b");
    CHECK(a[2] == "-DX=it's");
    CHECK(a[3] == "-I/p q");
    CHECK(a[5] == "x.cpp");
    CHECK(split_command_line(R"(c++ -DS=\"q\" "")").size() == 3);
}

HOTPP_TEST(compile_db_resolves_relative_files) {
    const CompileDb db = CompileDb::parse(R"([
        {"directory": "/build", "file": "../src/a.cpp", "command": "c++ -c ../src/a.cpp -o a.o"},
        {"directory": "/build", "file": "/src/b.cpp", "arguments": ["c++", "-c", "/src/b.cpp"]}
    ])", "/build/compile_commands.json");
    CHECK(db.entries().size() == 2);
    const CompileCommand* a = db.find("/src/a.cpp");
    CHECK(a != nullptr);
    CHECK(a && a->arguments.size() == 5);
    CHECK(db.find("/src/b.cpp") != nullptr);
    CHECK(db.find("/src/c.cpp") == nullptr);
}

HOTPP_TEST(patch_arguments_redirect_output_and_drop_depfiles) {
    CompileCommand cmd;
    cmd.arguments = {"c++", "-O2", "-MD", "-MT", "x.o", "-MF", "x.o.d", "-MFother.d", "-o", "x.o", "-c", "x.cpp"};
    const auto out = patch_compile_arguments(cmd, "/tmp/p/x.o");
    CHECK(!has(out, "-MD"));
    CHECK(!has(out, "-MT"));
    CHECK(!has(out, "-MF"));
    CHECK(!has(out, "-MFother.d"));
    CHECK(!has(out, "x.o"));
    CHECK(!has(out, "x.o.d"));
    CHECK(has(out, "-O2"));
    CHECK(has(out, "-c"));
    CHECK(has(out, "x.cpp"));
    CHECK(has(out, "-ffunction-sections"));
    CHECK(has(out, "-fdata-sections"));
    CHECK(out.size() >= 2 && out[out.size() - 2] == "-o" && out.back() == "/tmp/p/x.o");

    cmd.arguments = {"c++", "-ox.o", "-c", "x.cpp"};
    const auto joined = patch_compile_arguments(cmd, "/tmp/y.o");
    CHECK(!has(joined, "-ox.o"));
}
