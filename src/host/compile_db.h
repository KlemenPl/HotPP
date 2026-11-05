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

// Splits a shell command line (POSIX quoting rules) into arguments.
std::vector<std::string> split_command_line(const std::string& command);

// The original arguments with the output redirected to `output`, dependency-file
// flags removed (they would overwrite the build's own) and the flags patches rely
// on added.
std::vector<std::string> patch_compile_arguments(const CompileCommand& cmd, const std::filesystem::path& output);

// Absolute, lexically normal form used for all path comparisons.
std::filesystem::path normalize_path(const std::filesystem::path& p, const std::filesystem::path& base = {});

} // namespace hotpp
