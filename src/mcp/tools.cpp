#include "mcp/tools.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace hotpp::mcp {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string instructions() {
    return "HotPP hot-reloads C++ programs on Linux: it recompiles changed source files with the build's own "
           "commands and patches them into the running process, which keeps its state (globals, heap, threads).\n"
           "- hotpp_status lists the programs running under HotPP. hotpp_launch starts one; it must be built with "
           "HotPP (hotpp_enable() in CMake, or the single header) and call hotpp::init().\n"
           "- After editing a running program's .cpp files or headers, call hotpp_reload. It applies every change "
           "as one patch and reports compile errors, changes that need a restart, or success. Programs started with "
           "hotpp_launch only change when you call hotpp_reload (auto-reload is off), so finish a multi-file edit "
           "first. Pass observe_ms (e.g. 2000) to also see what the program prints right after the patch. Don't "
           "rebuild with the build system to apply an edit; that needs a restart.\n"
           "- A layout warning (a struct or class gained or lost members) means objects created before the patch "
           "have the old layout: restart unless none of them is still in use.\n"
           "- If a change can't be patched (restart required: e.g. a global changed size or type), call "
           "hotpp_restart: it rebuilds and restarts the program with the same pid; program state is lost.\n"
           "- hotpp_output shows what a launched program printed (wait_ms watches it for a while); hotpp_logs shows "
           "the host's messages and patch "
           "history. hotpp_stop ends a launched program. Launched programs stop when this server exits.";
}

// ---- arguments ---------------------------------------------------------------

struct HotppTools::Param {
    enum Type { Integer, Boolean, String, StringArray, StringMap };
    const char* name;
    Type type;
    const char* description;
    bool required = false;
    int64_t min = 0, max = 0; // integers
};

class HotppTools::Args {
public:
    explicit Args(const json::Value& v) : v_(v) {}
    bool has(const char* name) const {
        const json::Value* x = v_.get(name);
        return x && !x->is_null();
    }
    int64_t integer(const char* name, int64_t fallback) const {
        return has(name) ? int64_t(std::llround(v_.get(name)->as_number())) : fallback;
    }
    std::optional<uint32_t> pid() const {
        if (!has("pid")) return std::nullopt;
        return uint32_t(integer("pid", 0));
    }
    bool boolean(const char* name, bool fallback) const { return has(name) ? v_.get(name)->as_bool() : fallback; }
    std::string string(const char* name, const std::string& fallback = {}) const {
        return has(name) ? v_.get(name)->as_string() : fallback;
    }
    std::vector<std::string> strings(const char* name) const {
        std::vector<std::string> out;
        if (has(name))
            for (const json::Value& s : v_.get(name)->as_array()) out.push_back(s.as_string());
        return out;
    }
    std::vector<std::pair<std::string, std::string>> map(const char* name) const {
        std::vector<std::pair<std::string, std::string>> out;
        if (has(name))
            for (const auto& [k, v] : v_.get(name)->as_object()) out.emplace_back(k, v.as_string());
        return out;
    }

private:
    const json::Value& v_;
};

struct HotppTools::Tool {
    const char* name;
    const char* title;
    const char* description;
    std::vector<Param> params;
    bool read_only = false, destructive = false, idempotent = false;
    ToolResult (HotppTools::*run)(const Args&, CallContext&);
};

namespace {

using Param = HotppTools::Param;

json::Value schema(const std::vector<Param>& params) {
    json::Value props = json::Value::object();
    json::Value required = json::Value::array();
    for (const Param& p : params) {
        json::Value s = json::Value::object();
        switch (p.type) {
        case Param::Integer:
            s.set("type", "integer");
            if (p.max > p.min) s.set("minimum", p.min).set("maximum", p.max);
            break;
        case Param::Boolean: s.set("type", "boolean"); break;
        case Param::String: s.set("type", "string"); break;
        case Param::StringArray: {
            json::Value items = json::Value::object();
            items.set("type", "string");
            s.set("type", "array").set("items", std::move(items));
            if (p.required) s.set("minItems", 1);
            break;
        }
        case Param::StringMap: {
            json::Value values = json::Value::object();
            values.set("type", "string");
            s.set("type", "object").set("additionalProperties", std::move(values));
            break;
        }
        }
        s.set("description", p.description);
        props.set(p.name, std::move(s));
        if (p.required) required.push(p.name);
    }
    json::Value v = json::Value::object();
    v.set("type", "object").set("properties", std::move(props));
    if (required.size()) v.set("required", std::move(required));
    v.set("additionalProperties", false);
    return v;
}

// Checks `args` against the tool's parameters; an error message, or empty.
std::string validate(const HotppTools::Tool& tool, const json::Value& args) {
    auto names = [&] {
        std::string s;
        for (const Param& p : tool.params) s += (s.empty() ? "" : ", ") + std::string(p.name);
        return s.empty() ? std::string("no arguments") : s;
    };
    for (const auto& [key, value] : args.as_object()) {
        const auto p = std::find_if(tool.params.begin(), tool.params.end(), [&](const Param& x) { return key == x.name; });
        if (p == tool.params.end())
            return "unknown argument '" + key + "'; " + tool.name + " takes " + names();
        if (value.is_null()) continue;
        const std::string what = std::string("'") + p->name + "' must be ";
        switch (p->type) {
        case Param::Integer: {
            if (!value.is_number() || std::floor(value.as_number()) != value.as_number()) return what + "an integer";
            if (p->max > p->min && (value.as_number() < double(p->min) || value.as_number() > double(p->max)))
                return what + "between " + std::to_string(p->min) + " and " + std::to_string(p->max);
            break;
        }
        case Param::Boolean:
            if (!value.is_bool()) return what + "true or false";
            break;
        case Param::String:
            if (!value.is_string()) return what + "a string";
            break;
        case Param::StringArray:
            if (!value.is_array()) return what + "an array of strings";
            for (const json::Value& s : value.as_array())
                if (!s.is_string()) return what + "an array of strings";
            if (p->required && value.as_array().empty()) return what + "a non-empty array of strings";
            break;
        case Param::StringMap:
            if (!value.is_object()) return what + "an object of strings";
            for (const auto& [k, v] : value.as_object())
                if (!v.is_string() || k.empty() || k.find('=') != std::string::npos)
                    return what + "an object mapping variable names to string values";
            break;
        }
    }
    for (const Param& p : tool.params) {
        const json::Value* v = args.get(p.name);
        if (p.required && (!v || v->is_null())) return std::string("'") + p.name + "' is required";
    }
    return {};
}

// ---- rendering ---------------------------------------------------------------

const json::Value* field(const json::Value& v, const char* name) {
    return v.is_object() ? v.get(name) : nullptr;
}
std::string str(const json::Value& v, const char* name, const std::string& fallback = {}) {
    const json::Value* f = field(v, name);
    return f && f->is_string() ? f->as_string() : fallback;
}
int64_t num(const json::Value& v, const char* name, int64_t fallback = 0) {
    const json::Value* f = field(v, name);
    return f && f->is_number() ? int64_t(f->as_number()) : fallback;
}
bool flag(const json::Value& v, const char* name, bool fallback = false) {
    const json::Value* f = field(v, name);
    return f && f->is_bool() ? f->as_bool() : fallback;
}
std::vector<std::string> strs(const json::Value& v, const char* name) {
    std::vector<std::string> out;
    if (const json::Value* f = field(v, name); f && f->is_array())
        for (const json::Value& s : f->as_array())
            if (s.is_string()) out.push_back(s.as_string());
    return out;
}

std::string plural(int64_t n, const char* word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

std::string basenames(const std::vector<std::string>& files, size_t max = 8) {
    std::string s;
    for (size_t i = 0; i < files.size() && i < max; ++i)
        s += (i ? ", " : "") + fs::path(files[i]).filename().string();
    if (files.size() > max) s += " and " + std::to_string(files.size() - max) + " more";
    return s;
}

std::string program_name(const Hosts::Program& p) {
    const std::string exe = str(p.info, "exe");
    if (!exe.empty()) return fs::path(exe).filename().string();
    if (p.launched && !p.launched->command.empty()) return fs::path(p.launched->command[0]).filename().string();
    if (const json::Value* child = field(p.host_info, "child"))
        if (const json::Value* cmd = field(*child, "command"); cmd && cmd->is_array() && cmd->size())
            if (cmd->as_array()[0].is_string()) return fs::path(cmd->as_array()[0].as_string()).filename().string();
    return "program";
}

std::string fenced(const std::string& text) {
    std::string t = text;
    while (!t.empty() && t.back() == '\n') t.pop_back();
    return "```\n" + t + "\n```";
}

// One patch: its line, warnings, notes and (optionally) the compiler output.
std::string render_report(const json::Value& r, bool with_output = true) {
    std::string out = str(r, "message", "patch " + std::to_string(num(r, "id")) + ": " + str(r, "status"));
    const auto headers = strs(r, "headers");
    if (!headers.empty()) out += " [changed headers: " + basenames(headers) + "]";
    if (str(r, "trigger") == "watch") out += " [applied automatically when the file was saved]";
    for (const std::string& w : strs(r, "warnings")) out += "\nwarning: " + w;
    for (const std::string& n : strs(r, "notes")) out += "\nnote: " + n;
    const std::string compiler = str(r, "compiler_output");
    if (with_output && !compiler.empty()) {
        out += "\n\nCompiler output" + std::string(flag(r, "compiler_output_truncated") ? " (shortened)" : "") + ":\n" +
               fenced(compiler);
    }
    return out;
}

// A program's state in a line or two.
std::string describe(const Hosts::Program& p) {
    std::string s = "pid " + std::to_string(p.pid) + " " + program_name(p);
    if (p.launched) s += " (launched by hotpp_launch)";
    if (p.exited) {
        const json::Value* child = field(p.host_info, "child");
        s += ": exited with code " + std::to_string(child ? num(*child, "exit_code", -1) : -1) +
             "; hotpp_output shows what it printed, hotpp_stop dismisses it";
        return s;
    }
    const std::string state = str(p.info, "state");
    if (state == "unavailable") return s + ": connected, but hot reload is unavailable: " + str(p.info, "error");
    if (state == "starting") return s + ": setting up (reading its symbols)";
    if (state == "restarting") return s + ": restarting";
    if (state == "disconnected") return s + ": disconnected";
    s += ": watching " + plural(num(p.info, "sources"), "source file");
    if (flag(p.info, "dependency_scan_done")) s += " and " + plural(num(p.info, "headers"), "header");
    else s += " (still scanning headers)";
    s += "; auto-reload " + std::string(flag(p.info, "auto_reload", true) ? "on" : "off");
    if (state == "patching") s += "; patching now";
    if (flag(p.info, "waiting_for_sync_point")) s += "; a patch is waiting for the program to call hotpp::sync_point()";
    const auto pending = strs(p.info, "pending");
    const auto failed = strs(p.info, "failed_files");
    if (pending.empty() && failed.empty()) s += "; runs the sources on disk";
    if (!pending.empty()) s += "; changed but not patched yet: " + basenames(pending) + " (call hotpp_reload)";
    if (!failed.empty()) s += "; last patch of " + basenames(failed) + " failed";
    return s;
}

ToolResult error(std::string text, json::Value structured = {}) {
    ToolResult r;
    r.is_error = true;
    r.text = std::move(text);
    r.structured = std::move(structured);
    return r;
}

ToolResult ok(std::string text, json::Value structured = {}) {
    ToolResult r;
    r.text = std::move(text);
    r.structured = std::move(structured);
    return r;
}

std::chrono::milliseconds ms(int64_t v) { return std::chrono::milliseconds(v); }
constexpr auto kSlack = 10s; // on top of a host-side timeout, for the reply to arrive

std::string time_of_day(int64_t unix_ms) {
    const std::time_t t = std::time_t(unix_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(unix_ms % 1000));
    return buf;
}

std::string build_command_of(const json::Value& host) { return str(host, "build_command"); }

} // namespace

// ---- the tools ---------------------------------------------------------------

HotppTools::HotppTools(Hosts& hosts, ToolsOptions options) : hosts_(hosts), options_(std::move(options)) {
    const Param pid{"pid", Param::Integer,
                    "Process id of the program (from hotpp_status). Optional when only one program is connected.",
                    false, 1, INT32_MAX};
    tools_ = {
        {"hotpp_status", "HotPP status",
         "List the programs running under HotPP hot reload, with what each is watching, whether it runs the "
         "sources on disk, and its last patch.",
         {},
         true, false, true, &HotppTools::status},
        {"hotpp_reload", "Apply source changes",
         "Recompile every source file and header changed since the last patch and patch them into the running "
         "program as one patch. Reports success, compile errors (with the compiler output), or that a restart is "
         "needed. Call it after editing a running program's sources; with observe_ms it also shows what the "
         "program prints right after the patch.",
         {pid,
          {"observe_ms", Param::Integer,
           "After a successful patch, wait this long and include what the program printed meanwhile (programs "
           "started with hotpp_launch). E.g. 2000.",
           false, 0, 60000},
          {"timeout_ms", Param::Integer, "How long to wait for the patch (default 60000).", false, 0, 600000}},
         false, false, false, &HotppTools::reload},
        {"hotpp_launch", "Launch a program",
         "Start a program under HotPP hot reload and wait until it's ready to be patched. It must be built with "
         "HotPP (hotpp_enable() in CMake, or the single header) and call hotpp::init(). Its output is kept for "
         "hotpp_output. Auto-reload is off by default: changes apply when you call hotpp_reload.",
         {{"command", Param::StringArray, "The program and its arguments, e.g. [\"./build/game\", \"--level\", \"2\"].",
           true},
          {"cwd", Param::String, "Working directory (default: the directory hotpp mcp runs in)."},
          {"env", Param::StringMap, "Extra environment variables."},
          {"build_command", Param::String,
           "Shell command that rebuilds the program, for hotpp_restart (e.g. \"cmake --build build\"). Defaults to "
           "hotpp mcp --build."},
          {"auto_restart", Param::Boolean, "Rebuild and restart by itself when a change can't be patched (default false)."},
          {"auto_reload", Param::Boolean, "Patch files as soon as they are saved (default false)."},
          {"timeout_ms", Param::Integer, "How long to wait for the program to be ready (default 30000).", false, 0,
           600000}},
         false, false, false, &HotppTools::launch},
        {"hotpp_restart", "Rebuild and restart",
         "Rebuild the program with its host's build command (if it has one) and restart it in place: same pid, "
         "same arguments, program state reset. Use it when hotpp_reload says a restart is required.",
         {pid,
          {"rebuild", Param::Boolean, "Run the build command first (default true)."},
          {"observe_ms", Param::Integer,
           "After the restart, wait this long and include what the program printed meanwhile (programs started "
           "with hotpp_launch).",
           false, 0, 60000},
          {"timeout_ms", Param::Integer, "How long to wait for the build and the restart (default 300000).", false, 0,
           600000}},
         false, true, false, &HotppTools::restart},
        {"hotpp_stop", "Stop a launched program",
         "Stop a program started with hotpp_launch (SIGTERM, then SIGKILL), or dismiss one that already exited. "
         "Programs started any other way are left alone.",
         {pid, {"timeout_ms", Param::Integer, "How long to wait for it to exit (default 10000).", false, 0, 600000}},
         false, true, true, &HotppTools::stop},
        {"hotpp_output", "Program output",
         "What a program started with hotpp_launch printed (stdout and stderr). Without 'since', returns the end of "
         "the output; pass the returned cursor as 'since' to read only what's new. With 'wait_ms', watches the "
         "program for that long and returns what it printed meanwhile.",
         {pid,
          {"since", Param::Integer, "Cursor from an earlier call: return output after it.", false, 0, INT64_MAX},
          {"wait_ms", Param::Integer,
           "Wait this long first (ends early if the program exits) and return what it printed during the wait, or "
           "since 'since'. E.g. 3000 to watch it run for 3 seconds.",
           false, 0, 60000},
          {"max_bytes", Param::Integer, "At most this much text (default 16384).", false, 1, 1 << 20},
          {"tail_lines", Param::Integer, "Only the last N lines (without 'since').", false, 1, 100000}},
         true, false, false, &HotppTools::output},
        {"hotpp_logs", "Host log",
         "Messages from the program's HotPP host: compiling, patches applied or rejected, warnings, restarts. Pass "
         "the returned cursor as 'since' to get only new ones, with 'wait_ms' to wait for them.",
         {pid,
          {"since", Param::Integer, "Cursor from an earlier call: return events after it.", false, 0, INT64_MAX},
          {"wait_ms", Param::Integer, "With 'since': wait up to this long for a new event (default 0).", false, 0,
           600000},
          {"limit", Param::Integer, "At most this many events (default 100).", false, 1, 1000}},
         true, false, false, &HotppTools::logs},
        {"hotpp_set_auto_reload", "Set auto-reload",
         "Turn auto-reload on (every saved file is patched right away) or off (changes wait for hotpp_reload). On "
         "programs not launched here, the setting lasts until this server exits.",
         {pid, {"enabled", Param::Boolean, "true: patch on save; false: only on hotpp_reload.", true}},
         false, false, true, &HotppTools::set_auto_reload},
    };
}

HotppTools::~HotppTools() = default;

json::Value HotppTools::list() const {
    json::Value out = json::Value::array();
    for (const Tool& t : tools_) {
        json::Value ann = json::Value::object();
        ann.set("title", t.title).set("readOnlyHint", t.read_only).set("destructiveHint", t.destructive);
        ann.set("idempotentHint", t.idempotent).set("openWorldHint", false);
        json::Value v = json::Value::object();
        v.set("name", t.name).set("title", t.title).set("description", t.description);
        v.set("inputSchema", schema(t.params)).set("annotations", std::move(ann));
        out.push(std::move(v));
    }
    return out;
}

bool HotppTools::has(const std::string& name) const {
    return std::any_of(tools_.begin(), tools_.end(), [&](const Tool& t) { return name == t.name; });
}

ToolResult HotppTools::call(const std::string& name, const json::Value& arguments, CallContext& ctx) {
    const auto t = std::find_if(tools_.begin(), tools_.end(), [&](const Tool& x) { return name == x.name; });
    if (t == tools_.end()) return error("unknown tool " + name);
    const std::string problem = validate(*t, arguments);
    if (!problem.empty()) return error("Invalid arguments: " + problem + ".");
    ToolResult result;
    try {
        result = (this->*(t->run))(Args(arguments), ctx);
    } catch (const HostError& e) {
        result = error(ctx.is_cancelled() ? "cancelled" : e.what());
    }
    if (!options_.structured) result.structured = json::Value();
    return result;
}

ToolResult HotppTools::status(const Args&, CallContext&) {
    std::vector<Hosts::Entry> hosts;
    const std::vector<Hosts::Program> programs = hosts_.programs(&hosts);
    std::string text;
    json::Value list = json::Value::array();
    for (const Hosts::Program& p : programs) {
        text += "- " + describe(p) + "\n";
        if (p.info.is_object())
            if (const json::Value* last = field(p.info, "last_patch")) text += "  last patch: " + render_report(*last, false) + "\n";
        text += "  host: pid " + std::to_string(num(p.host_info, "pid", -1)) + ", " + str(p.host_info, "endpoint") + "\n";
        json::Value v = json::Value::object();
        v.set("pid", p.pid).set("name", program_name(p)).set("launched", p.launched != nullptr).set("exited", p.exited);
        if (p.info.is_object()) v.set("session", p.info);
        v.set("host", p.host_info);
        list.push(std::move(v));
    }
    std::string idle;
    json::Value host_list = json::Value::array();
    for (const Hosts::Entry& e : hosts) {
        json::Value h = json::Value::object();
        h.set("endpoint", e.endpoint).set("reachable", e.client != nullptr).set("launched", e.launched != nullptr);
        if (!e.error.empty()) h.set("error", e.error);
        if (e.client) h.set("host", e.client->hello());
        host_list.push(std::move(h));
        if (e.incompatible) idle += "- a host at " + e.endpoint + " that can't be controlled: " + e.error + "\n";
        else if (!e.error.empty()) idle += "- a host at " + e.endpoint + " didn't answer: " + e.error + "\n";
    }
    std::string head;
    if (programs.empty())
        head = "No program is running under a HotPP host" + std::string(hosts.empty() ? "" : " (hosts found: " +
                                                                                             std::to_string(hosts.size()) + ")") +
               ". Start one with hotpp_launch, or run it under the host (hotpp -- <program>).\n";
    else
        head = plural(int64_t(programs.size()), "program") + " under HotPP:\n";
    json::Value structured = json::Value::object();
    structured.set("programs", std::move(list)).set("hosts", std::move(host_list));
    return ok(head + text + (idle.empty() ? "" : "\nOther hosts:\n" + idle), std::move(structured));
}

ToolResult HotppTools::reload(const Args& a, CallContext& ctx) {
    const Hosts::Program p = hosts_.find(a.pid());
    const int64_t timeout = a.integer("timeout_ms", 60000);
    const int64_t host_pid = p.host->host_pid();
    json::Value params = json::Value::object();
    params.set("pid", p.pid).set("timeout_ms", timeout).set("since_seq", hosts_.seen(p.host->endpoint(), host_pid, p.pid));
    const json::Value r = p.host->request("reload", std::move(params), ms(timeout) + kSlack, ctx.progress, ctx.cancelled);

    // Remember what was reported, so the next reload only mentions what's new.
    uint64_t newest = 0;
    if (const json::Value* rep = field(r, "report")) newest = std::max<uint64_t>(newest, uint64_t(num(*rep, "seq")));
    if (const json::Value* rec = field(r, "recent"); rec && rec->is_array())
        for (const json::Value& x : rec->as_array()) newest = std::max<uint64_t>(newest, uint64_t(num(x, "seq")));
    if (newest) hosts_.set_seen(p.host->endpoint(), host_pid, p.pid, newest);

    const std::string outcome = str(r, "outcome");
    const json::Value* report = field(r, "report");
    std::string text;
    bool failed = false;
    std::string recent_text;
    if (const json::Value* rec = field(r, "recent"); rec && rec->is_array() && rec->size()) {
        recent_text = "\n\nEarlier patches since your last reload:";
        for (const json::Value& x : rec->as_array()) recent_text += "\n- " + render_report(x, str(x, "status") != "applied");
    }
    const std::string name = program_name(p);
    const std::string details = report ? render_report(*report) : std::string("(no details)");
    if (outcome == "applied") {
        text = details + "\n\n" + name + " now runs your changes.";
    } else if (outcome == "up_to_date") {
        text = "Nothing to patch: " + name + " already runs the sources on disk.";
    } else if (outcome == "out_of_sync") {
        failed = true;
        text = "Nothing changed since the last attempt, which failed:\n" + details +
               "\n\nFix the problem and call hotpp_reload again; " + name + " keeps running the previous code.";
    } else if (outcome == "compile_failed" || outcome == "link_failed" || outcome == "not_applied") {
        failed = true;
        text = details + "\n\nNot applied: " + name +
               " keeps running the previous code. Fix the problem and call hotpp_reload again.";
    } else if (outcome == "restart_required") {
        text = details;
        if (flag(r, "restarted")) {
            text += "\n\nThe host rebuilt and restarted " + name + " (same pid); its state was reset.";
        } else if (report && flag(*report, "restarting")) {
            failed = true;
            text += "\n\nThe host is rebuilding and restarting " + name + "; check hotpp_status shortly.";
        } else {
            failed = true;
            text += "\n\nThis change can't be patched into the running program. Call hotpp_restart to rebuild and "
                    "restart it (its state is reset), or undo the change.";
        }
    } else if (outcome == "failed_in_app") {
        failed = true;
        text = details + "\n\nThe program rejected the patch and keeps running the previous code.";
    } else if (outcome == "waiting_for_sync_point") {
        text = (report ? render_report(*report) + "\n\n" : std::string()) +
               "The patch is compiled and queued, but " + name +
               " hasn't called hotpp::sync_point() for 2 seconds (paused in a debugger, blocked, or not calling it). "
               "It applies as soon as the program gets there; check with hotpp_status.";
    } else if (outcome == "restarting") {
        failed = true;
        text = name + " is restarting; call hotpp_reload again once hotpp_status shows it watching.";
    } else if (outcome == "timeout") {
        failed = true;
        text = "No result within " + std::to_string(timeout) + " ms; the host is still working on it (big "
               "translation units take a while). Call hotpp_reload again, or check hotpp_logs.";
    } else if (outcome == "unavailable" || outcome == "disconnected") {
        failed = true;
        text = name + ": " + str(r, "error", outcome);
    } else {
        failed = true;
        text = "Reload ended with '" + outcome + "'" + (str(r, "error").empty() ? "" : ": " + str(r, "error"));
    }
    if (outcome != "applied" && outcome != "up_to_date" && !flag(r, "in_sync", true) && outcome != "out_of_sync" &&
        outcome != "waiting_for_sync_point")
        text += "\n(" + name + " doesn't run all sources on disk.)";
    if (report && outcome == "applied")
        for (const std::string& w : strs(*report, "warnings"))
            if (w.rfind("layout of ", 0) == 0) {
                text += "\n\nImportant: a struct or class changed layout. The patched code now uses the new layout, "
                        "also on objects created before the patch (globals, objects on the heap, locals of functions "
                        "still running such as main), which can corrupt memory. Unless no such object exists, call "
                        "hotpp_restart now.";
                break;
            }
    text += recent_text;
    if (outcome == "applied" || (outcome == "restart_required" && flag(r, "restarted")))
        text += observe(p, a.integer("observe_ms", 0), ctx);
    json::Value structured = r;
    return failed ? error(text, std::move(structured)) : ok(text, std::move(structured));
}

ToolResult HotppTools::launch(const Args& a, CallContext& ctx) {
    LaunchSpec spec;
    spec.command = a.strings("command");
    spec.cwd = a.string("cwd", options_.cwd);
    if (!spec.cwd.empty() && fs::path(spec.cwd).is_relative()) spec.cwd = (fs::path(options_.cwd) / spec.cwd).string();
    std::error_code ec;
    if (!spec.cwd.empty() && !fs::is_directory(spec.cwd, ec)) return error("cwd '" + spec.cwd + "' is not a directory.");
    for (const auto& [k, v] : a.map("env")) spec.env.push_back(k + "=" + v);
    spec.build_command = a.string("build_command", options_.default_build);
    spec.auto_restart = a.boolean("auto_restart", false);
    spec.auto_reload = a.boolean("auto_reload", false);
    const int64_t timeout = a.integer("timeout_ms", 30000);
    std::string cmdline;
    for (const std::string& s : spec.command) cmdline += (cmdline.empty() ? "" : " ") + s;

    ctx.progress("starting " + cmdline);
    std::shared_ptr<HostClient> client;
    std::shared_ptr<Launched> launched;
    try {
        launched = hosts_.launch(spec, client, ms(timeout));
    } catch (const HostError& e) {
        return error("Couldn't start " + cmdline + ": " + e.what());
    }
    json::Value params = json::Value::object();
    params.set("timeout_ms", timeout);
    json::Value r;
    try {
        r = client->request("wait_program", std::move(params), ms(timeout) + kSlack, ctx.progress, ctx.cancelled);
    } catch (const HostError& e) {
        std::string out = launched->host_output();
        hosts_.forget(launched);
        if (out.size() > 8192) out = out.substr(out.size() - 8192);
        return error("Couldn't start " + cmdline + ": " + e.what() + (out.empty() ? "" : "\n\nHost output:\n" + fenced(out)));
    }
    const json::Value host = field(r, "host") ? *field(r, "host") : json::Value::object();
    const json::Value* child = field(host, "child");
    const int64_t child_pid = child ? num(*child, "pid") : 0;

    // What it printed so far.
    std::string printed;
    try {
        json::Value op = json::Value::object();
        op.set("tail_lines", 40).set("max_bytes", 8192);
        printed = str(client->request("output", std::move(op), 5s), "text");
    } catch (const HostError&) {
    }
    auto with_output = [&](std::string text) {
        if (!printed.empty()) text += "\n\nOutput so far:\n" + fenced(printed);
        return text;
    };
    json::Value structured = r;
    structured.set("command", cmdline);

    if (child && field(*child, "exit_code")) { // it exited (not: it hasn't started yet)
        const std::string text = with_output(cmdline + " exited with code " + std::to_string(num(*child, "exit_code", -1)) +
                                             " before HotPP could patch it.");
        hosts_.forget(launched);
        return error(text, std::move(structured));
    }
    if (!flag(r, "ready")) {
        return error(with_output(cmdline + " is running (pid " + std::to_string(child_pid) +
                                 "), but no HotPP agent connected within " + std::to_string(timeout) +
                                 " ms. Make sure it's built with HotPP (hotpp_enable() in CMake, or the single header) "
                                 "and calls hotpp::init() early. It keeps running; hotpp_stop stops it."),
                     std::move(structured));
    }
    Hosts::Program p;
    p.info = *field(r, "program");
    p.host_info = host;
    p.launched = launched;
    p.pid = uint32_t(num(p.info, "pid"));
    if (str(p.info, "state") == "unavailable")
        return error(with_output(describe(p) + "\nIt keeps running; hotpp_stop stops it."), std::move(structured));
    std::string text = "Started " + cmdline + ": " + describe(p) + ".";
    text += spec.auto_reload ? "\nAuto-reload is on: saved files are patched right away; hotpp_reload reports the result."
                             : "\nAuto-reload is off: edit the sources, then call hotpp_reload to patch them in.";
    if (spec.build_command.empty())
        text += "\nNo build command is set, so hotpp_restart restarts without rebuilding (pass build_command to "
                "rebuild first).";
    return ok(with_output(text), std::move(structured));
}

ToolResult HotppTools::restart(const Args& a, CallContext& ctx) {
    const Hosts::Program p = hosts_.find(a.pid());
    const int64_t timeout = a.integer("timeout_ms", 300000);
    const bool rebuild = a.boolean("rebuild", true);
    json::Value params = json::Value::object();
    params.set("pid", p.pid).set("rebuild", rebuild).set("timeout_ms", timeout);
    const json::Value r = p.host->request("restart", std::move(params), ms(timeout) + kSlack, ctx.progress, ctx.cancelled);
    const std::string outcome = str(r, "outcome");
    const std::string name = program_name(p);
    const json::Value host = field(r, "host") ? *field(r, "host") : p.host_info;
    const bool built = rebuild && !build_command_of(host).empty();
    if (outcome == "restarted") {
        Hosts::Program now = p;
        now.info = *field(r, "program");
        std::string text = (built ? "Rebuilt and restarted " : "Restarted ") + name + ": " + describe(now) + ".";
        if (rebuild && !built)
            text += "\nThe host has no build command, so it restarted the program as built (pass build_command to "
                    "hotpp_launch, or start hotpp mcp with --build, to rebuild first).";
        text += observe(p, a.integer("observe_ms", 0), ctx);
        return ok(text, r);
    }
    if (outcome == "build_failed")
        return error("The build failed (" + str(r, "error") + "); " + name + " keeps running unchanged.\n\nBuild output:\n" +
                         fenced(str(r, "build_output")),
                     r);
    if (outcome == "exited") return error(name + " exited instead of coming back after the restart. " +
                                              (p.launched ? "hotpp_output shows what it printed." : ""),
                                          r);
    return error("Restart of " + name + " ended with '" + outcome + "'" +
                     (str(r, "error").empty() ? "" : ": " + str(r, "error")),
                 r);
}

ToolResult HotppTools::stop(const Args& a, CallContext& ctx) {
    const Hosts::Program p = hosts_.find(a.pid(), true);
    if (!p.launched)
        return error("pid " + std::to_string(p.pid) + " (" + program_name(p) +
                     ") wasn't started with hotpp_launch, so it's left alone. Stop it the way it was started.");
    const int64_t timeout = a.integer("timeout_ms", 10000);
    json::Value params = json::Value::object();
    params.set("timeout_ms", timeout);
    const json::Value r = p.host->request("stop", std::move(params), ms(timeout) + kSlack, {}, ctx.cancelled);
    const std::string name = program_name(p);
    if (!flag(r, "stopped")) return error(name + " didn't exit within " + std::to_string(timeout) + " ms.", r);
    hosts_.forget(p.launched);
    const std::string code = std::to_string(num(r, "exit_code", -1));
    return ok(flag(r, "was_running") ? "Stopped " + name + " (pid " + std::to_string(p.pid) + ", exit code " + code + ")."
                                     : name + " had already exited (code " + code + "); dismissed it.",
              r);
}

// What the program prints during the next `ms_to_watch` ms, for appending to a result.
std::string HotppTools::observe(const Hosts::Program& p, int64_t ms_to_watch, CallContext& ctx) {
    if (ms_to_watch <= 0) return {};
    if (!flag(p.host_info, "captures_output"))
        return "\n\n(observe_ms: output isn't captured for programs not started with hotpp_launch)";
    json::Value params = json::Value::object();
    params.set("wait_ms", ms_to_watch).set("max_bytes", 8192);
    try {
        const json::Value r = p.host->request("output", std::move(params), ms(ms_to_watch) + kSlack, {}, ctx.cancelled);
        const std::string text = str(r, "text");
        std::string out = "\n\nOutput over the next " + std::to_string(ms_to_watch) + " ms:\n" +
                          (text.empty() ? std::string("(nothing)") : fenced(text));
        if (num(r, "next") - num(r, "start") >= 8192) out += "\n(more followed; hotpp_output shows the rest)";
        return out + "\n[pass since=" + std::to_string(num(r, "next")) + " to hotpp_output to read on]";
    } catch (const HostError& e) {
        return std::string("\n\n(couldn't read the output: ") + e.what() + ")";
    }
}

ToolResult HotppTools::output(const Args& a, CallContext& ctx) {
    const Hosts::Program p = hosts_.find(a.pid(), true);
    if (!flag(p.host_info, "captures_output"))
        return error("Output of pid " + std::to_string(p.pid) + " (" + program_name(p) +
                     ") isn't captured: only programs started with hotpp_launch are. Its output goes wherever it "
                     "was started.");
    json::Value params = json::Value::object();
    params.set("max_bytes", a.integer("max_bytes", 16384));
    if (a.has("since")) params.set("since", a.integer("since", 0));
    else if (a.has("tail_lines")) params.set("tail_lines", a.integer("tail_lines", 0));
    const int64_t wait = a.integer("wait_ms", 0);
    if (wait) params.set("wait_ms", wait);
    const json::Value r = p.host->request("output", std::move(params), ms(wait) + kSlack, {}, ctx.cancelled);
    std::string text = str(r, "text");
    const int64_t start = num(r, "start"), next = num(r, "next"), dropped = num(r, "dropped");
    std::string footer = "[output bytes " + std::to_string(start) + "-" + std::to_string(next);
    if (dropped) footer += "; " + std::to_string(dropped) + " earlier bytes are no longer kept";
    if (flag(r, "closed") || p.exited) footer += "; the program has exited";
    footer += "; pass since=" + std::to_string(next) + " to read on]";
    if (text.empty()) text = a.has("since") ? "(nothing new)" : "(no output yet)";
    else text = fenced(text);
    return ok(text + "\n" + footer, r);
}

ToolResult HotppTools::logs(const Args& a, CallContext& ctx) {
    const Hosts::Program p = hosts_.find(a.pid(), true);
    json::Value params = json::Value::object();
    params.set("pid", p.exited ? 0 : p.pid).set("limit", a.integer("limit", 100));
    const int64_t wait = a.integer("wait_ms", 0);
    if (a.has("since")) params.set("since", a.integer("since", 0)).set("wait_ms", wait);
    const json::Value r = p.host->request("events", std::move(params), ms(wait) + kSlack, {}, ctx.cancelled);
    std::string text;
    if (const json::Value* events = field(r, "events"); events && events->is_array()) {
        for (const json::Value& e : events->as_array()) {
            std::string line = "[" + time_of_day(num(e, "time_ms")) + "] ";
            if (const json::Value* rep = field(e, "patch")) line += render_report(*rep, str(*rep, "status") != "applied");
            else line += str(e, "text");
            text += line + "\n";
        }
    }
    if (text.empty()) text = a.has("since") ? "(no new events)\n" : "(no events yet)\n";
    if (flag(r, "truncated")) text = "(older events are no longer kept)\n" + text;
    text += "[pass since=" + std::to_string(num(r, "next")) + " to get only newer events]";
    return ok(text, r);
}

ToolResult HotppTools::set_auto_reload(const Args& a, CallContext&) {
    const Hosts::Program p = hosts_.find(a.pid());
    const bool on = a.boolean("enabled", false);
    json::Value params = json::Value::object();
    params.set("pid", p.pid).set("enabled", on);
    const json::Value r = p.host->request("set_auto_reload", std::move(params), 10s);
    std::string text = "Auto-reload is " + std::string(on ? "on" : "off") + " for " + program_name(p) + " (pid " +
                       std::to_string(p.pid) + "): " +
                       (on ? "saved files are patched right away." : "changes wait for hotpp_reload.");
    if (!p.launched) text += " This lasts until hotpp mcp exits; then the host's own setting applies again.";
    return ok(text, r);
}

} // namespace hotpp::mcp
