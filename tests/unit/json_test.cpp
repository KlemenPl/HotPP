#include <cmath>
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

HOTPP_TEST(json_builds_and_dumps) {
    Value v = Value::object();
    v.set("n", 42).set("neg", int64_t(-7)).set("d", 1.5).set("b", true).set("s", "x\"y\\z\n").set("nul", nullptr);
    Value a = Value::array();
    a.push(1).push("two").push(Value::object());
    v.set("a", std::move(a));
    v.set("n", 43); // replaces
    CHECK(v.dump() == R"({"n":43,"neg":-7,"d":1.5,"b":true,"s":"x\"y\\z\n","nul":null,"a":[1,"two",{}]})");
    const Value back = Value::parse(v.dump());
    CHECK(back.get("n")->is_integer() && back.get("n")->as_integer() == 43);
    CHECK(back.get("s")->as_string() == "x\"y\\z\n");
    CHECK(back.get("a")->as_array().size() == 3);
}

HOTPP_TEST(json_integers_round_trip_exactly) {
    const Value v = Value::parse(R"([9007199254740993, -1, 0, 1.0, 1e3, 12345678901234567890])");
    const auto& a = v.as_array();
    CHECK(a[0].is_integer() && a[0].as_integer() == 9007199254740993LL); // beyond double precision
    CHECK(a[1].as_integer() == -1);
    CHECK(!a[3].is_integer() && !a[4].is_integer());
    CHECK(!a[5].is_integer()); // too big for int64: a double
    CHECK(Value(uint64_t(5)).dump() == "5");
    CHECK(Value(std::nan("")).dump() == "null");
    CHECK(Value::parse("[9007199254740993]").dump() == "[9007199254740993]");
}

HOTPP_TEST(json_dump_is_valid_utf8_whatever_the_input) {
    // Invalid sequences (a lone continuation byte, a truncated sequence, an overlong
    // form, a surrogate) become U+FFFD; valid ones and control characters survive.
    const std::string bad = std::string("a\x80") + "b\xE2\x82" + "c\xC0\xAF" + "d\xED\xA0\x80" + "e\x01\x7F" +
                            "\xF0\x9F\x98\x80";
    const std::string out = Value(bad).dump();
    CHECK(out == "\"a\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD" "c\xEF\xBF\xBD\xEF\xBF\xBD" "d\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                 "e\\u0001\\u007f\xF0\x9F\x98\x80\"");
    CHECK(Value::parse(out).is_string());
    CHECK(hotpp::json::valid_utf8("ok\xFF") == "ok\xEF\xBF\xBD");
}

HOTPP_TEST(json_rejects_deep_nesting) {
    CHECK(throws(std::string(300, '[').c_str()));
    std::string ok(200, '[');
    ok += std::string(200, ']');
    CHECK(!throws(ok.c_str()));
}
