#include "host/util/json.h"
#include "unit/test.h"

using hotpp::json::JsonError;
using hotpp::json::Value;

namespace {
bool throws(const char* text) {
    try {
        Value::parse(text);
    } catch (const JsonError&) {
        return true;
    }
    return false;
}
} // namespace

HOTPP_TEST(json_parses_compile_commands_shape) {
    const Value v = Value::parse(R"([
        {"directory": "/b", "file": "a.cpp", "arguments": ["c++", "-c", "a.cpp"], "n": -1.5e2, "ok": true},
        {"directory": "/b", "command": "c++ -c b.cpp", "file": "b.cpp", "x": null}
    ])");
    CHECK(v.is_array());
    CHECK(v.as_array().size() == 2);
    const Value& e = v.as_array()[0];
    CHECK(e.get("directory")->as_string() == "/b");
    CHECK(e.get("arguments")->as_array().size() == 3);
    CHECK(e.get("n")->as_number() == -150.0);
    CHECK(e.get("ok")->as_bool());
    CHECK(e.get("missing") == nullptr);
    CHECK(v.as_array()[1].get("x")->type() == Value::Type::Null);
}

HOTPP_TEST(json_string_escapes) {
    const Value v = Value::parse(R"(["a\"b\\c\/d\n\t", "é€", "😀"])");
    CHECK(v.as_array()[0].as_string() == "a\"b\\c/d\n\t");
    CHECK(v.as_array()[1].as_string() == "\xc3\xa9\xe2\x82\xac");
    CHECK(v.as_array()[2].as_string() == "\xf0\x9f\x98\x80");
}

HOTPP_TEST(json_rejects_malformed_input) {
    CHECK(throws(""));
    CHECK(throws("[1, 2"));
    CHECK(throws("{\"a\" 1}"));
    CHECK(throws("[1] x"));
    CHECK(throws("\"unterminated"));
    CHECK(throws("[\"bad \\q escape\"]"));
}
