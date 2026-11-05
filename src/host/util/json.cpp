#include "host/util/json.h"

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

const std::vector<Value>& Value::as_array() const {
    if (type_ != Type::Array) throw JsonError("expected a JSON array");
    return array_;
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
            if (!end || *end) fail("bad number");
            v.type_ = Value::Type::Number;
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
