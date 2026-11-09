// compile_commands.json: how each translation unit was compiled, and how to
// recompile it for a patch.
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace hotpp {

struct CompileCommand {
    std::filesystem::path directory;
    std::filesystem::path file; // absolute, normalized
    std::vector<std::string> arguments;
};

class CompileDb {
public:
    // Throws std::runtime_error if the file can't be read or parsed.
    static CompileDb load(const std::filesystem::path& path);
    static CompileDb parse(const std::string& json, const std::filesystem::path& origin);

    const std::filesystem::path& path() const { return path_; }
    const std::vector<CompileCommand>& entries() const { return entries_; }
    const CompileCommand* find(const std::filesystem::path& file) const;

private:
    std::filesystem::path path_;
    std::vector<CompileCommand> entries_;
    std::unordered_map<std::string, size_t> by_file_;
};

// Replaces @file arguments (resolved against `directory`) with the arguments in them.
std::vector<std::string> expand_response_files(const std::vector<std::string>& args,
                                               const std::filesystem::path& directory, int depth = 0);

// Splits a command line into arguments: POSIX shell quoting, or with `windows` the
// rules of CommandLineToArgvW (what compile_commands.json uses on Windows).
std::vector<std::string> split_command_line(const std::string& command, bool windows = false);
// Splits with the rules of the system HotPP runs on.
std::vector<std::string> split_native_command_line(const std::string& command);

// How a compiler takes its arguments: gcc/clang style (-o, -MF), or MSVC style
// (cl.exe, and clang-cl, which accepts cl's syntax).
enum class Driver { Gnu, Cl, ClangCl };
Driver driver_of(const CompileCommand& cmd);
inline bool msvc_style(Driver d) { return d != Driver::Gnu; }

// The original arguments with the output redirected to `output`, the compiler
// told to write the TU's dependencies to `depfile` (see read_dependencies), the
// build's own dependency-file flags removed (they would overwrite its files) and
// the flags patches rely on added. Without `depfile`, no dependencies are written.
std::vector<std::string> patch_compile_arguments(const CompileCommand& cmd, const std::filesystem::path& output,
                                                 const std::filesystem::path& depfile = {});

// Absolute, lexically normal form used for all path comparisons.
std::filesystem::path normalize_path(const std::filesystem::path& p, const std::filesystem::path& base = {});

} // namespace hotpp
