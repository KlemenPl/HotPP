// Minimal JSON reader, enough for compile_commands.json. Throws JsonError.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hotpp::json {

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    const std::string& as_string() const;
    double as_number() const;
    bool as_bool() const;
    const std::vector<Value>& as_array() const;
    // nullptr if this is not an object or has no such key.
    const Value* get(std::string_view key) const;

    static Value parse(std::string_view text);

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

} // namespace hotpp::json
