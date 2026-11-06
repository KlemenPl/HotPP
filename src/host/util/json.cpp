#include "host/util/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hotpp::json {

const std::string& Value::as_string() const {
    if (type_ != Type::String) throw JsonError("expected a JSON string");
    return string_;
}

double Value::as_number() const {
    if (type_ != Type::Number) throw JsonError("expected a JSON number");
    return number_;
}

bool Value::as_bool() const {
    if (type_ != Type::Bool) throw JsonError("expected a JSON boolean");
    return bool_;
}

int64_t Value::as_integer() const {
    if (!is_integer()) throw JsonError("expected an integer");
    return int_;
}

const std::vector<Value>& Value::as_array() const {
    if (type_ != Type::Array) throw JsonError("expected a JSON array");
    return array_;
}

const std::vector<std::pair<std::string, Value>>& Value::as_object() const {
    if (type_ != Type::Object) throw JsonError("expected a JSON object");
    return object_;
}

Value& Value::set(std::string key, Value v) {
    if (type_ == Type::Null) type_ = Type::Object;
    if (type_ != Type::Object) throw JsonError("set() on a JSON value that is not an object");
    for (auto& [k, old] : object_)
        if (k == key) {
            old = std::move(v);
            return *this;
        }
    object_.emplace_back(std::move(key), std::move(v));
    return *this;
}

Value& Value::push(Value v) {
    if (type_ == Type::Null) type_ = Type::Array;
    if (type_ != Type::Array) throw JsonError("push() on a JSON value that is not an array");
    array_.push_back(std::move(v));
    return *this;
}

size_t Value::size() const {
    return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0;
}

namespace {

// Length of the valid UTF-8 sequence at s[i], or 0 if it isn't one (overlong
// forms, surrogates and code points above U+10FFFF are invalid).
size_t utf8_sequence(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return uint8_t(s[i + k]); };
    const uint8_t c = b(0);
    if (c < 0x80) return 1;
    size_t n;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1F;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0F;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07;
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(k) & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    return n;
}

constexpr const char kReplacement[] = "\xEF\xBF\xBD"; // U+FFFD

} // namespace

std::string valid_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_sequence(s, i);
        if (n == 0) {
            out += kReplacement;
            ++i;
        } else {
            out.append(s.data() + i, n);
            i += n;
        }
    }
    return out;
}

void append_string(std::string& out, std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const uint8_t u = uint8_t(c);
        if (u >= 0x80) {
            const size_t n = utf8_sequence(s, i);
            if (n == 0) {
                out += kReplacement;
                ++i;
            } else {
                out.append(s.data() + i, n);
                i += n;
            }
            continue;
        }
        ++i;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (u < 0x20 || u == 0x7F) {
                out += "\\u00";
                out += hex[u >> 4];
                out += hex[u & 15];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void Value::dump_to(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
        if (integer_) {
            out += std::to_string(int_);
        } else if (!std::isfinite(number_)) {
            out += "null"; // JSON has no NaN or infinity
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", number_);
            out += buf;
        }
        break;
    }
    case Type::String: append_string(out, string_); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < array_.size(); ++i) {
            if (i) out += ',';
            array_[i].dump_to(out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < object_.size(); ++i) {
            if (i) out += ',';
            append_string(out, object_[i].first);
            out += ':';
            object_[i].second.dump_to(out);
        }
        out += '}';
        break;
    }
}

std::string Value::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

const Value* Value::get(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : object_)
        if (k == key) return &v;
    return nullptr;
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value document() {
        Value v = value(0);
        ws();
        if (pos_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw JsonError(std::string("invalid JSON at offset ") + std::to_string(pos_) + ": " + what);
    }

    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    Value value(int depth) {
        if (depth > 256) fail("nesting too deep");
        ws();
        if (pos_ >= s_.size()) fail("unexpected end");
        Value v;
        const char c = s_[pos_];
        if (c == '{') {
            v.type_ = Value::Type::Object;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return v;
            }
            for (;;) {
                ws();
                if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected a key");
                std::string key = string();
                ws();
                if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':'");
                ++pos_;
                v.object_.emplace_back(std::move(key), value(depth + 1));
                ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return v; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type_ = Value::Type::Array;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return v;
            }
            for (;;) {
                v.array_.push_back(value(depth + 1));
                ws();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return v; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type_ = Value::Type::String;
            v.string_ = string();
            return v;
        }
        if (consume("true")) { v.type_ = Value::Type::Bool; v.bool_ = true; return v; }
        if (consume("false")) { v.type_ = Value::Type::Bool; return v; }
        if (consume("null")) return v;
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = pos_;
            while (pos_ < s_.size() && std::string_view("+-0123456789.eE").find(s_[pos_]) != std::string_view::npos) ++pos_;
            const std::string num(s_.substr(start, pos_ - start));
            char* end = nullptr;
            v.number_ = std::strtod(num.c_str(), &end);
            if (!end || *end || num == "-") fail("bad number");
            v.type_ = Value::Type::Number;
            if (num.find_first_of(".eE") == std::string::npos) {
                errno = 0;
                const long long n = std::strtoll(num.c_str(), &end, 10);
                if (errno == 0 && end && !*end) {
                    v.integer_ = true;
                    v.int_ = n;
                }
            }
            return v;
        }
        fail("unexpected character");
    }

    unsigned hex4() {
        if (pos_ + 4 > s_.size()) fail("short \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        ++pos_; // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && consume("\\u")) {
                    const unsigned lo = hex4();
                    if (lo < 0xDC00 || lo >= 0xE000) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }

    std::string_view s_;
    size_t pos_ = 0;
};

Value Value::parse(std::string_view text) { return Parser(text).document(); }

} // namespace hotpp::json
