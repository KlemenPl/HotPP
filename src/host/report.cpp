#include "host/report.h"

namespace hotpp::host {

namespace {
json::Value strings(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (const std::string& s : v) a.push(s);
    return a;
}
} // namespace

json::Value PatchReport::to_json() const {
    json::Value v = json::Value::object();
    v.set("id", id).set("pid", pid).set("seq", seq).set("trigger", trigger).set("status", status);
    v.set("files", strings(files));
    if (!headers.empty()) v.set("headers", strings(headers));
    v.set("message", message);
    if (!error.empty()) v.set("error", error);
    if (!compiler_output.empty()) {
        v.set("compiler_output", compiler_output);
        if (compiler_output_truncated) v.set("compiler_output_truncated", true);
    }
    if (!warnings.empty()) v.set("warnings", strings(warnings));
    if (!notes.empty()) v.set("notes", strings(notes));
    if (restarting) v.set("restarting", true);
    v.set("duration_ms", duration_ms);
    if (applied()) {
        json::Value c = json::Value::object();
        c.set("functions_replaced", functions_replaced)
            .set("functions_new", functions_new)
            .set("variables_kept", variables_kept)
            .set("initializers_run", initializers_run)
            .set("bytes", bytes)
            .set("modules", modules);
        v.set("counts", std::move(c));
    }
    return v;
}

std::string strip_ansi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c != '\033') {
            out += c;
            ++i;
            continue;
        }
        // ESC [ params intermediates final (CSI), ESC ] ... BEL or ESC \ (OSC), or
        // ESC followed by one character.
        ++i;
        if (i >= s.size()) break;
        if (s[i] == '[') {
            ++i;
            while (i < s.size() && uint8_t(s[i]) >= 0x20 && uint8_t(s[i]) <= 0x3F) ++i; // parameters, intermediates
            if (i < s.size()) ++i;                                                     // final byte
        } else if (s[i] == ']') {
            ++i;
            while (i < s.size() && s[i] != '\a' && !(s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\')) ++i;
            if (i < s.size()) i += s[i] == '\a' ? 1 : 2;
        } else {
            ++i;
        }
    }
    return out;
}

std::string cap_output(std::string_view s, size_t head, size_t tail, bool* truncated) {
    if (truncated) *truncated = false;
    if (s.size() <= head + tail) return std::string(s);
    if (truncated) *truncated = true;
    size_t h = head;
    if (const size_t nl = s.rfind('\n', head); nl != std::string_view::npos && nl > head / 2) h = nl + 1;
    size_t t = s.size() - tail;
    if (const size_t nl = s.find('\n', t); nl != std::string_view::npos && nl < t + tail / 2) t = nl + 1;
    std::string out(s.substr(0, h));
    out += "\n[... " + std::to_string(t - h) + " bytes omitted ...]\n";
    out += s.substr(t);
    return out;
}

} // namespace hotpp::host
