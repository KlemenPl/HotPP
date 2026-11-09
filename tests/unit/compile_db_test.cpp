#include "host/compile_db.h"
#include "unit/test.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <unistd.h>

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

HOTPP_TEST(response_files_are_expanded) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("hotpp-rsp-" + std::to_string(::getpid()));
    fs::create_directories(dir / "sub");
    {
        std::ofstream(dir / "args.rsp") << "-O2 \"-DNAME=a b\" @sub/more.rsp -o x.o";
        std::ofstream(dir / "sub/more.rsp") << "-MD -MF x.o.d";
    }
    const CompileDb db = CompileDb::parse(R"([{"directory": ")" + dir.string() +
                                              R"(", "file": "x.cpp", "command": "c++ @args.rsp -c x.cpp"}])",
                                          dir / "compile_commands.json");
    const auto& a = db.entries().at(0).arguments;
    CHECK(has(a, "-DNAME=a b"));
    CHECK(has(a, "-MD")); // from the nested file
    CHECK(!has(a, "@args.rsp"));
    const auto patched = patch_compile_arguments(db.entries()[0], "/tmp/p.o");
    CHECK(!has(patched, "x.o")); // the -o inside the response file was rewritten
    CHECK(!has(patched, "-MD"));
    fs::remove_all(dir);
}

HOTPP_TEST(split_command_line_windows_rules) {
    const auto a = split_command_line(R"(C:\VS\cl.exe /nologo "/DNAME=a b" /DQ=\"x\" "C:\dir with space\\" /Fo"out dir\x.obj" C:\src\x.cpp)",
                                      true);
    CHECK(a.size() == 7);
    CHECK(a.size() == 7 && a[0] == R"(C:\VS\cl.exe)");
    CHECK(a.size() == 7 && a[2] == "/DNAME=a b");
    CHECK(a.size() == 7 && a[3] == "/DQ=\"x\"");
    CHECK(a.size() == 7 && a[4] == R"(C:\dir with space\)"); // 2n backslashes before a quote: n of them
    CHECK(a.size() == 7 && a[5] == R"(/Foout dir\x.obj)");
    CHECK(a.size() == 7 && a[6] == R"(C:\src\x.cpp)");
}

HOTPP_TEST(driver_is_detected) {
    CompileCommand cmd;
    cmd.arguments = {"/usr/bin/c++", "-c", "x.cpp"};
    CHECK(driver_of(cmd) == Driver::Gnu);
    cmd.arguments = {"C:\\VS\\bin\\CL.EXE", "/c", "x.cpp"};
    CHECK(driver_of(cmd) == Driver::Cl);
    cmd.arguments = {"/usr/lib/llvm-19/bin/clang-cl", "/c", "x.cpp"};
    CHECK(driver_of(cmd) == Driver::ClangCl);
    cmd.arguments = {"clang++", "--driver-mode=cl", "/c", "x.cpp"};
    CHECK(driver_of(cmd) == Driver::ClangCl);
}

HOTPP_TEST(msvc_patch_arguments) {
    CompileCommand cmd;
    cmd.arguments = {"cl.exe", "/nologo", "/Zi", "/FdC:\\b\\x.pdb", "/FoC:\\b\\x.obj", "/showIncludes", "/O2",
                     "/sourceDependencies", "C:\\b\\x.json", "/c", "C:\\s\\x.cpp"};
    const auto out = patch_compile_arguments(cmd, "C:\\p\\x.obj", "C:\\p\\x.json");
    CHECK(!has(out, "/Zi"));
    CHECK(!has(out, "/FdC:\\b\\x.pdb"));
    CHECK(!has(out, "/FoC:\\b\\x.obj"));
    CHECK(!has(out, "/showIncludes"));
    CHECK(!has(out, "C:\\b\\x.json"));
    CHECK(has(out, "/O2"));
    CHECK(has(out, "/c"));
    CHECK(has(out, "/Z7"));
    CHECK(has(out, "/Gy"));
    CHECK(has(out, "/Gw"));
    CHECK(has(out, "/FoC:\\p\\x.obj"));
    CHECK(has(out, "/sourceDependencies"));
    CHECK(has(out, "C:\\p\\x.json"));

    // clang-cl: options stay before `--`, dependencies through /clang:.
    cmd.arguments = {"clang-cl", "/nologo", "-Fox.obj", "/Z7", "/c", "--", "/home/s/x.cpp"};
    const auto c = patch_compile_arguments(cmd, "/p/x.obj", "/p/x.d");
    CHECK(!has(c, "-Fox.obj"));
    CHECK(has(c, "/clang:-MMD"));
    CHECK(has(c, "/clang:-MF/p/x.d"));
    CHECK(c.back() == "/home/s/x.cpp");
    CHECK(c.size() >= 2 && c[c.size() - 2] == "--");
    CHECK(std::count(c.begin(), c.end(), "/Z7") == 1);
}

HOTPP_TEST(utf16_response_files_are_read) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("hotpp-rsp16-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "args.rsp", std::ios::binary);
        const std::string text = "-O2 -DX=1";
        f.put(char(0xFF));
        f.put(char(0xFE));
        for (char ch : text) {
            f.put(ch);
            f.put(0);
        }
    }
    const CompileDb db = CompileDb::parse(R"([{"directory": ")" + dir.string() +
                                              R"(", "file": "x.cpp", "arguments": ["c++", "@args.rsp", "-c", "x.cpp"]}])",
                                          dir / "compile_commands.json");
    const auto& a = db.entries().at(0).arguments;
    CHECK(has(a, "-O2"));
    CHECK(has(a, "-DX=1"));
    fs::remove_all(dir);
}
