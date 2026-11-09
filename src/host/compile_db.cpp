#include "host/compile_db.h"

#include "host/util/json.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hotpp {

namespace fs = std::filesystem;

fs::path normalize_path(const fs::path& p, const fs::path& base) {
    fs::path abs = p.is_absolute() || base.empty() ? p : base / p;
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(abs, ec);
    return (ec ? abs : canon).lexically_normal();
}

namespace {

// CommandLineToArgvW: whitespace separates arguments; inside or outside quotes,
// 2n backslashes before a quote are n backslashes and the quote toggles quoting,
// 2n+1 are n backslashes and a literal quote; other backslashes are literal.
std::vector<std::string> split_windows(const std::string& command) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false, quoted = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            if (have) out.push_back(std::move(cur));
            cur.clear();
            have = false;
            continue;
        }
        have = true;
        if (c == '\\') {
            size_t n = 0;
            while (i < command.size() && command[i] == '\\') ++n, ++i;
            if (i < command.size() && command[i] == '"') {
                cur.append(n / 2, '\\');
                if (n % 2) {
                    cur += '"';
                } else {
                    quoted = !quoted;
                }
            } else {
                cur.append(n, '\\');
                --i;
            }
        } else if (c == '"') {
            // "" inside quotes is a literal quote (as the MSVC runtime reads it).
            if (quoted && i + 1 < command.size() && command[i + 1] == '"') {
                cur += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else {
            cur += c;
        }
    }
    if (have) out.push_back(std::move(cur));
    return out;
}

// Response files written by Windows tools may be UTF-16 with a byte order mark.
std::string response_file_text(const std::string& raw) {
    if (raw.size() < 2 || uint8_t(raw[0]) != 0xFF || uint8_t(raw[1]) != 0xFE) {
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) return raw.substr(3);
        return raw;
    }
    std::string out;
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        uint32_t c = uint8_t(raw[i]) | (uint32_t(uint8_t(raw[i + 1])) << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
            const uint32_t lo = uint8_t(raw[i + 2]) | (uint32_t(uint8_t(raw[i + 3])) << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace

std::vector<std::string> split_native_command_line(const std::string& command) {
#ifdef _WIN32
    return split_command_line(command, true);
#else
    return split_command_line(command, false);
#endif
}

std::vector<std::string> split_command_line(const std::string& command, bool windows) {
    if (windows) return split_windows(command);
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            if (have) out.push_back(std::move(cur));
            cur.clear();
            have = false;
        } else if (c == '\\' && i + 1 < command.size()) {
            cur += command[++i];
            have = true;
        } else if (c == '\'') {
            have = true;
            while (++i < command.size() && command[i] != '\'') cur += command[i];
        } else if (c == '"') {
            have = true;
            while (++i < command.size() && command[i] != '"') {
                if (command[i] == '\\' && i + 1 < command.size() &&
                    std::string("\"\\$`\n").find(command[i + 1]) != std::string::npos)
                    ++i;
                cur += command[i];
            }
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(std::move(cur));
    return out;
}

// Splices @file arguments in, as the compiler would read them, so the output and
// dependency flags inside a response file can be rewritten too. A response file
// that can't be read is left as is.
std::vector<std::string> expand_response_files(const std::vector<std::string>& args, const fs::path& directory,
                                               int depth) {
    std::vector<std::string> out;
    for (const std::string& a : args) {
        if (a.size() < 2 || a[0] != '@' || depth > 8) {
            out.push_back(a);
            continue;
        }
        std::ifstream in(normalize_path(a.substr(1), directory), std::ios::binary);
        if (!in) {
            out.push_back(a);
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        for (std::string& e : expand_response_files(split_native_command_line(response_file_text(ss.str())), directory, depth + 1))
            out.push_back(std::move(e));
    }
    return out;
}

CompileDb CompileDb::load(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), path);
}

CompileDb CompileDb::parse(const std::string& text, const fs::path& origin) {
    CompileDb db;
    db.path_ = origin;
    const json::Value doc = json::Value::parse(text);
    for (const json::Value& e : doc.as_array()) {
        const json::Value* dir = e.get("directory");
        const json::Value* file = e.get("file");
        if (!dir || !file) throw std::runtime_error(origin.string() + ": entry without directory/file");
        CompileCommand cmd;
        cmd.directory = normalize_path(dir->as_string());
        cmd.file = normalize_path(file->as_string(), cmd.directory);
        if (const json::Value* args = e.get("arguments")) {
            for (const json::Value& a : args->as_array()) cmd.arguments.push_back(a.as_string());
        } else if (const json::Value* command = e.get("command")) {
            cmd.arguments = split_native_command_line(command->as_string());
        }
        if (cmd.arguments.empty()) throw std::runtime_error(origin.string() + ": entry for " + cmd.file.string() + " has no command");
        cmd.arguments = expand_response_files(cmd.arguments, cmd.directory, 0);
        db.by_file_.emplace(cmd.file.string(), db.entries_.size());
        db.entries_.push_back(std::move(cmd));
    }
    return db;
}

const CompileCommand* CompileDb::find(const fs::path& file) const {
    auto it = by_file_.find(normalize_path(file).string());
    return it == by_file_.end() ? nullptr : &entries_[it->second];
}

Driver driver_of(const CompileCommand& cmd) {
    if (cmd.arguments.empty()) return Driver::Gnu;
    // Either separator: a Windows compiler path may be read on Linux (cross builds, tests).
    const std::string& prog = cmd.arguments[0];
    const size_t sep = prog.find_last_of("/\\");
    std::string name = sep == std::string::npos ? prog : prog.substr(sep + 1);
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
    if (name == "cl") return Driver::Cl;
    if (name == "clang-cl" || name.rfind("clang-cl-", 0) == 0) return Driver::ClangCl;
    for (const std::string& a : cmd.arguments)
        if (a == "--driver-mode=cl") return Driver::ClangCl;
    return Driver::Gnu;
}

namespace {

// An MSVC-style option: /name or -name.
bool msvc_flag(const std::string& s, const char* name) {
    return (s[0] == '/' || s[0] == '-') && s.compare(1, std::string::npos, name) == 0;
}
bool msvc_flag_prefix(const std::string& s, const char* name) {
    return s.size() > 1 && (s[0] == '/' || s[0] == '-') && s.compare(1, std::strlen(name), name) == 0;
}

// cl / clang-cl arguments without the output, dependency and debug-format options
// (all of which patches set themselves).
std::vector<std::string> msvc_base_arguments(const CompileCommand& cmd) {
    std::vector<std::string> out;
    const auto& a = cmd.arguments;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string& s = a[i];
        if (i > 0 && !s.empty()) {
            if (msvc_flag(s, "sourceDependencies") || msvc_flag(s, "clang:-MF") || msvc_flag(s, "clang:-MT")) {
                ++i; // the flag and its value
                continue;
            }
            if (msvc_flag_prefix(s, "Fo") || msvc_flag_prefix(s, "Fd") || msvc_flag_prefix(s, "Fa") ||
                msvc_flag_prefix(s, "FA") || msvc_flag_prefix(s, "sourceDependencies") || msvc_flag(s, "showIncludes") ||
                msvc_flag_prefix(s, "showIncludes:") || msvc_flag(s, "Zi") || msvc_flag(s, "ZI") || msvc_flag(s, "Z7") ||
                msvc_flag_prefix(s, "clang:-M"))
                continue;
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

std::vector<std::string> patch_compile_arguments(const CompileCommand& cmd, const fs::path& output,
                                                 const fs::path& depfile) {
    const Driver driver = driver_of(cmd);
    if (msvc_style(driver)) {
        std::vector<std::string> out = msvc_base_arguments(cmd);
        // The source file is last in what CMake writes; options go before it (cl
        // reads options after a file too, but `--` would end them for clang-cl).
        auto at = out.end();
        for (auto it = out.begin(); it != out.end(); ++it)
            if (*it == "--") {
                at = it;
                break;
            }
        // /Z7: debug info in the object, which the patch linker reads (no shared
        // compiler PDB to contend for). /Gy /Gw: one section per function and per
        // variable (see below).
        // Forward slashes: cl and clang-cl accept them, and a native clang-cl run from
        // a host under Wine needs them.
        // /hotpatch: clang-cl only; cl takes it on x86 only (x64 code is hot-patchable as is).
        std::vector<std::string> extra = {"/Z7", "/Gy", "/Gw", "/Fo" + output.generic_string()};
        if (driver == Driver::ClangCl) extra.push_back("/hotpatch");
        if (!depfile.empty()) {
            if (driver == Driver::Cl) {
                extra.insert(extra.end(), {"/sourceDependencies", depfile.generic_string()});
            } else {
                extra.insert(extra.end(), {"/clang:-MMD", "/clang:-MF" + depfile.generic_string()});
            }
        }
        out.insert(at, extra.begin(), extra.end());
        return out;
    }
    std::vector<std::string> out;
    const auto& a = cmd.arguments;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string& s = a[i];
        if (s == "-o" || s == "-MF" || s == "-MT" || s == "-MQ") {
            ++i; // drop the flag and its value
            continue;
        }
        if (s == "-MD" || s == "-MMD" || s == "-MP" || s.rfind("-MF", 0) == 0 || s.rfind("-MT", 0) == 0 ||
            s.rfind("-MQ", 0) == 0)
            continue;
        if (s.size() > 2 && s.rfind("-o", 0) == 0) continue; // -ofile
        out.push_back(s);
    }
    // One section per function and per variable lets the patch linker keep each
    // existing variable where the running program has it.
    out.push_back("-ffunction-sections");
    out.push_back("-fdata-sections");
    out.push_back("-o");
    out.push_back(output.string());
    if (!depfile.empty()) out.insert(out.end(), {"-MMD", "-MF", depfile.string()});
    return out;
}

} // namespace hotpp
