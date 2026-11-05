#include "host/compile_db.h"

#include "host/util/json.h"

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

std::vector<std::string> split_command_line(const std::string& command) {
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
            cmd.arguments = split_command_line(command->as_string());
        }
        if (cmd.arguments.empty()) throw std::runtime_error(origin.string() + ": entry for " + cmd.file.string() + " has no command");
        db.by_file_.emplace(cmd.file.string(), db.entries_.size());
        db.entries_.push_back(std::move(cmd));
    }
    return db;
}

const CompileCommand* CompileDb::find(const fs::path& file) const {
    auto it = by_file_.find(normalize_path(file).string());
    return it == by_file_.end() ? nullptr : &entries_[it->second];
}

std::vector<std::string> patch_compile_arguments(const CompileCommand& cmd, const fs::path& output) {
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
    return out;
}

} // namespace hotpp
