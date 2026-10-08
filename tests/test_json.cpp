#include <clocale>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "anpr/common/json.hpp"
#include "test_framework.hpp"

using anpr::json::Value;

namespace {

/// Parses `text` and returns the value; a failed parse yields a value that fails every check.
Value parsed(const std::string& text) {
    const auto result = anpr::json::parse(text);
    return result.ok ? result.value : Value::string("<parse error: " + result.error + ">");
}

std::string nested(int depth) {
    return std::string(static_cast<std::size_t>(depth), '[') + "0" +
           std::string(static_cast<std::size_t>(depth), ']');
}

}  // namespace

TEST("json parses scalars") {
    CHECK(parsed("null").isNull());
    CHECK_EQ(parsed(" true ").asBool().value_or(false), true);
    CHECK_EQ(parsed("false").asBool().value_or(true), false);
    CHECK_EQ(parsed("0").asNumber().value_or(-1.0), 0.0);
    CHECK_EQ(parsed("-12").asInt().value_or(0), std::int64_t{-12});
    CHECK_NEAR(parsed("-3.5").asNumber().value_or(0.0), -3.5, 1e-12);
    CHECK_NEAR(parsed("1e3").asNumber().value_or(0.0), 1000.0, 1e-9);
    CHECK_NEAR(parsed("1.5E-2").asNumber().value_or(0.0), 0.015, 1e-15);
    CHECK_NEAR(parsed("2.5e+2").asNumber().value_or(0.0), 250.0, 1e-9);
    CHECK(std::signbit(parsed("-0").asNumber().value_or(1.0)));
    CHECK_EQ(parsed("9007199254740993").asNumber().value_or(0.0), 9007199254740992.0);
    CHECK_EQ(parsed("1e-400").asNumber().value_or(1.0), 0.0);  // Underflow rounds to zero.
    CHECK_EQ(parsed("\"camera-02\"").asString().value_or(""), std::string("camera-02"));
    CHECK_EQ(parsed("\"\"").asString().value_or("x"), std::string());
    CHECK(!parsed("1").asString().has_value());
    CHECK(!parsed("\"1\"").asNumber().has_value());
    CHECK(!parsed("true").asInt().has_value());
}

TEST("json parses nested documents and keeps member order") {
    const auto result = anpr::json::parse(
        "{\"cameras\": [ {\"id\": \"camera-01\", \"fps\": 24.5, \"ok\": true},\n"
        "\t{\"id\": \"camera-02\", \"fps\": 0, \"ok\": false, \"error\": null} ],\r\n"
        "  \"version\": 2, \"empty\": {}, \"none\": []}");
    CHECK(result.ok);
    CHECK(result.error.empty());
    const Value& root = result.value;
    CHECK(root.isObject());
    CHECK_EQ(root.members().size(), std::size_t{4});
    CHECK_EQ(root.members()[0].first, std::string("cameras"));
    CHECK_EQ(root.members()[3].first, std::string("none"));
    CHECK_EQ(root.getInt("version"), std::int64_t{2});
    const Value* cameras = root.find("cameras");
    CHECK(cameras != nullptr);
    CHECK(cameras->isArray());
    CHECK_EQ(cameras->items().size(), std::size_t{2});
    CHECK_EQ(cameras->items()[0].getString("id"), std::string("camera-01"));
    CHECK_NEAR(cameras->items()[0].getNumber("fps"), 24.5, 1e-12);
    CHECK(cameras->items()[0].getBool("ok"));
    CHECK(!cameras->items()[1].getBool("ok", true));
    CHECK(cameras->items()[1].find("error")->isNull());
    CHECK(root.find("empty")->isObject());
    CHECK(root.find("empty")->members().empty());
    CHECK(root.find("none")->isArray());
    CHECK(root.find("none")->items().empty());
    CHECK(root.find("missing") == nullptr);
}

TEST("json decodes escapes and unicode to UTF-8") {
    CHECK_EQ(parsed("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"").asString().value_or(""),
             std::string("\"\\/\b\f\n\r\t"));
    CHECK_EQ(parsed("\"\\u0041\\u00e9\\u20AC\"").asString().value_or(""),
             std::string("A\xc3\xa9\xe2\x82\xac"));
    // A surrogate pair is one code point (U+1F600), four UTF-8 bytes.
    CHECK_EQ(parsed("\"\\ud83d\\ude00\"").asString().value_or(""),
             std::string("\xf0\x9f\x98\x80"));
    // Lone surrogates cannot be represented and become U+FFFD; what follows is kept.
    CHECK_EQ(parsed("\"\\ud83dx\"").asString().value_or(""), std::string("\xef\xbf\xbdx"));
    CHECK_EQ(parsed("\"\\ude00\"").asString().value_or(""), std::string("\xef\xbf\xbd"));
    CHECK_EQ(parsed("\"\\ud83d\\u0041\"").asString().value_or(""),
             std::string("\xef\xbf\xbd" "A"));
    CHECK_EQ(parsed("\"\\u0000\"").asString().value_or("x"), std::string(1, '\0'));
    // Raw UTF-8 passes through untouched.
    CHECK_EQ(parsed("\"\xd0\x90\xd0\x9b\xd0\x9c\xd0\x90\xd0\xa2\xd0\xab\"").asString().value_or(""),
             std::string("\xd0\x90\xd0\x9b\xd0\x9c\xd0\x90\xd0\xa2\xd0\xab"));
}

TEST("json rejects malformed documents with a positioned error") {
    const std::vector<std::string> malformed = {
        "",          " ",           "{",           "[1,]",        "{\"a\":1,}", "01",
        "1.",        ".5",          "-",           "1e",          "1e+",        "+1",
        "tru",       "nul",         "falsey",      "\"open",      "\"\\x\"",    "\"\\u12G4\"",
        "\"\\u12\"", "\"a\x01z\"",  "[1 2]",       "{\"a\" 1}",   "{a:1}",      "1 2",
        "NaN",       "Infinity",    "-Infinity",   "1e400",       "-1e400",     "0x10",
        "[\"a\"]]",  "{\"a\":}",    "{,}",         "[,1]",        "\"\\",       "{\"a\":1 \"b\":2}",
    };
    for (const std::string& text : malformed) {
        const auto result = anpr::json::parse(text);
        CHECK(!result.ok);
        CHECK(result.value.isNull());
        CHECK(result.error.find("at offset") != std::string::npos);
    }
    const auto result = anpr::json::parse("[1, 2, x]");
    CHECK_EQ(result.error, std::string("unexpected character at offset 7"));
}

TEST("json limits nesting depth") {
    CHECK(anpr::json::parse(nested(200)).ok);
    const auto deep = anpr::json::parse(nested(100000));  // Must not overflow the stack.
    CHECK(!deep.ok);
    CHECK(deep.error.find("nested deeper") != std::string::npos);
    std::string objects;
    for (int i = 0; i < 1000; ++i) {
        objects += "{\"a\":";
    }
    CHECK(!anpr::json::parse(objects + "1" + std::string(1000, '}')).ok);
}

TEST("json dump writes compact escaped text that parses back") {
    Value root = Value::object();
    root.set("text",
             Value::string("quote\" backslash\\ newline\n tab\t ctrl\x01 slash/ utf8 \xc3\xa9"));
    root.set("integer", Value::number(25));
    root.set("negative", Value::number(-0.5));
    root.set("fraction", Value::number(0.1));
    root.set("third", Value::number(1.0 / 3.0));
    root.set("large", Value::number(123456789012345.0));
    root.set("huge", Value::number(1e300));
    root.set("tiny", Value::number(5e-324));
    root.set("nan", Value::number(std::numeric_limits<double>::quiet_NaN()));
    root.set("flag", Value::boolean(false));
    root.set("nothing", Value());
    Value list = Value::array();
    list.push(Value::number(1));
    list.push(Value::string("two"));
    list.push(Value::array());
    list.push(Value::object());
    root.set("list", list);

    const std::string text = root.dump();
    CHECK(text.find("\"integer\":25,") != std::string::npos);
    CHECK(text.find("\"negative\":-0.5,") != std::string::npos);
    CHECK(text.find("\"fraction\":0.1,") != std::string::npos);
    CHECK(text.find("\"large\":123456789012345,") != std::string::npos);
    CHECK(text.find("\"nan\":null,") != std::string::npos);
    CHECK(text.find("\"list\":[1,\"two\",[],{}]") != std::string::npos);
    CHECK(text.find("ctrl\\u0001") != std::string::npos);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.find(' ') != std::string::npos);  // Only inside strings.
    CHECK_EQ(text.front(), '{');
    CHECK_EQ(text.substr(0, 10), std::string("{\"text\":\"q"));

    const auto again = anpr::json::parse(text);
    CHECK(again.ok);
    CHECK_EQ(again.value.dump(), text);
    CHECK_EQ(again.value.getString("text"), root.getString("text"));
    CHECK(root.getString("text").size() > 40);
    CHECK_EQ(again.value.getNumber("third"), 1.0 / 3.0);  // Exact round trip.
    CHECK_EQ(again.value.getNumber("huge"), 1e300);
    CHECK_EQ(again.value.getNumber("tiny"), 5e-324);
    CHECK(again.value.find("nan")->isNull());

    CHECK_EQ(Value().dump(), std::string("null"));
    CHECK_EQ(Value::boolean(true).dump(), std::string("true"));
    CHECK_EQ(Value::number(-7).dump(), std::string("-7"));
    CHECK_EQ(Value::number(1e21).dump(), std::string("1e+21"));
    CHECK_EQ(Value::number(std::numeric_limits<double>::infinity()).dump(), std::string("null"));
    CHECK_EQ(Value::string("x").dump(), std::string("\"x\""));
}

TEST("json escape and quote") {
    CHECK_EQ(anpr::json::escape("a\"b\\c\n\r\t\b\f\x1f/\xc3\xa9"),
             std::string("a\\\"b\\\\c\\n\\r\\t\\b\\f\\u001f/\xc3\xa9"));
    CHECK_EQ(anpr::json::quote("rtsp://<redacted>@192.168.1.64:554/x"),
             std::string("\"rtsp://<redacted>@192.168.1.64:554/x\""));
    CHECK_EQ(anpr::json::quote(""), std::string("\"\""));
    CHECK_EQ(anpr::json::escape(std::string("nul\0byte", 8)), std::string("nul\\u0000byte"));
}

TEST("json accessors return fallbacks for missing or mistyped members") {
    const Value root = parsed("{\"s\":\"text\",\"n\":2.5,\"i\":-3,\"b\":true,\"big\":1e20,"
                              "\"z\":null}");
    CHECK_EQ(root.getString("s"), std::string("text"));
    CHECK_EQ(root.getString("n", "fallback"), std::string("fallback"));
    CHECK_EQ(root.getString("missing", "fallback"), std::string("fallback"));
    CHECK_NEAR(root.getNumber("n"), 2.5, 1e-12);
    CHECK_NEAR(root.getNumber("s", 7.0), 7.0, 1e-12);
    CHECK_EQ(root.getInt("i"), std::int64_t{-3});
    CHECK_EQ(root.getInt("n", 9), std::int64_t{9});  // 2.5 is not silently truncated.
    CHECK_EQ(root.getInt("big", 9), std::int64_t{9});  // Outside int64.
    CHECK(root.getBool("b"));
    CHECK(!root.getBool("z"));
    CHECK(root.getBool("missing", true));
    CHECK_EQ(Value::number(-9.2e18).asInt().value_or(0), std::int64_t{-9200000000000000000LL});
    CHECK(!Value::number(std::nan("")).asInt().has_value());

    const Value list = parsed("[1,2]");
    CHECK(list.find("a") == nullptr);
    CHECK_EQ(list.getString("a", "f"), std::string("f"));
    CHECK(list.members().empty());
    CHECK(root.items().empty());
}

TEST("json set replaces members, push and set convert the value kind") {
    Value object = Value::object();
    object.set("a", Value::number(1));
    object.set("b", Value::number(2));
    object.set("a", Value::string("replaced"));
    CHECK_EQ(object.members().size(), std::size_t{2});
    CHECK_EQ(object.members()[0].first, std::string("a"));
    CHECK_EQ(object.getString("a"), std::string("replaced"));

    // A repeated name in a document keeps one member with the last value.
    const Value duplicate = parsed("{\"k\":1,\"other\":0,\"k\":2}");
    CHECK_EQ(duplicate.members().size(), std::size_t{2});
    CHECK_EQ(duplicate.getInt("k"), std::int64_t{2});

    Value list;
    list.push(Value::number(1));
    CHECK(list.isArray());
    CHECK_EQ(list.items().size(), std::size_t{1});
    Value converted = Value::string("x");
    converted.set("k", Value::boolean(true));
    CHECK(converted.isObject());
    CHECK(converted.getBool("k"));
    CHECK_EQ(converted.kind(), Value::Kind::kObject);
}

TEST("json numbers ignore the process locale") {
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous != nullptr ? previous : "C";
    bool switched = false;
    for (const char* name :
         {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "ru_RU.UTF-8", "ru_RU.utf8"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            switched = true;
            break;
        }
    }
    const auto result = anpr::json::parse("{\"fps\": 12.5}");
    const std::string text = Value::number(0.25).dump();
    std::setlocale(LC_NUMERIC, saved.c_str());
    // Without a comma-decimal locale installed this still checks the C-locale behaviour.
    (void)switched;
    CHECK(result.ok);
    CHECK_NEAR(result.value.getNumber("fps"), 12.5, 1e-12);
    CHECK_EQ(text, std::string("0.25"));
}
