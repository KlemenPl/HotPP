// Small JSON value: parses compile_commands.json and MCP / control messages, and
// builds and serializes replies. Parsing throws JsonError.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hotpp::json {

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T n) : type_(Type::Number), number_(double(n)), integer_(true) {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(int64_t)) {
            if (n > T(INT64_MAX)) integer_ = false; // printed as a double
            int_ = int64_t(n);
        } else {
            int_ = int64_t(n);
        }
    }
    Value(double d) : type_(Type::Number), number_(d) {}
    Value(const char* s) : type_(Type::String), string_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}

    static Value object() {
        Value v;
        v.type_ = Type::Object;
        return v;
    }
    static Value array() {
        Value v;
        v.type_ = Type::Array;
        return v;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    // A number without a fraction or exponent that fits in int64_t.
    bool is_integer() const { return type_ == Type::Number && integer_; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    const std::string& as_string() const;
    double as_number() const;
    int64_t as_integer() const; // throws unless is_integer()
    bool as_bool() const;
    const std::vector<Value>& as_array() const;
    const std::vector<std::pair<std::string, Value>>& as_object() const;
    // nullptr if this is not an object or has no such key.
    const Value* get(std::string_view key) const;

    // Builders. set() replaces an existing key; both return *this.
    Value& set(std::string key, Value v);
    Value& push(Value v);
    size_t size() const; // elements of an array or members of an object

    // Compact JSON on one line. Invalid UTF-8 in strings becomes U+FFFD, so the
    // output is always valid JSON (program and compiler output can be anything).
    std::string dump() const;

    static Value parse(std::string_view text);

private:
    friend class Parser;
    void dump_to(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    bool integer_ = false;
    int64_t int_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Appends `s` as a JSON string literal (quotes included) to `out`.
void append_string(std::string& out, std::string_view s);
// `s` with invalid UTF-8 sequences replaced by U+FFFD.
std::string valid_utf8(std::string_view s);

} // namespace hotpp::json
