#include "core/Json.h"

#include <catch_amalgamated.hpp>

using ve::json::Value;

TEST_CASE("JSON round trip keeps types, int64 precision and key order", "[json]")
{
    Value doc = Value::object();
    doc.set("zeta", 1);
    doc.set("alpha", "texto com acentuação e \"aspas\"\n");
    doc.set("flicks", std::int64_t(60'963'840'000'000)); // 24 h in flicks
    doc.set("big", std::int64_t(9'007'199'254'740'993)); // 2^53 + 1: not representable as double
    doc.set("ratio", 0.5);
    doc.set("on", true);
    doc.set("nothing", nullptr);
    Value list = Value::array();
    list.push(1);
    list.push(Value::object());
    list.push(Value::array());
    doc.set("list", list);

    for (int indent : {0, 2}) {
        const std::string text = ve::json::serialize(doc, indent);
        std::string error;
        auto back = ve::json::parse(text, &error);
        INFO(text << error);
        REQUIRE(back.has_value());
        CHECK(*back == doc);
        CHECK((*back)["big"].asInt() == 9'007'199'254'740'993);
        CHECK(back->asObject().front().first == "zeta"); // insertion order kept
    }
}

TEST_CASE("JSON numbers are locale independent and keep their kind", "[json]")
{
    CHECK(ve::json::serialize(Value(0.5), 0) == "0.5");
    CHECK(ve::json::serialize(Value(2.0), 0) == "2.0");
    auto v = ve::json::parse("[1, -2, 1.5e3, -0.25]");
    REQUIRE(v);
    CHECK(v->asArray()[0].type() == Value::Type::Int);
    CHECK(v->asArray()[1].asInt() == -2);
    CHECK(v->asArray()[2].asDouble() == 1500.0);
    CHECK(v->asArray()[3].asDouble() == -0.25);
}

TEST_CASE("JSON strings decode escapes and surrogate pairs to UTF-8", "[json]")
{
    auto v = ve::json::parse(R"(["\u00e7\u00e3o", "\ud83c\udfac", "a\tb\\c\/d"])");
    REQUIRE(v);
    CHECK(v->asArray()[0].asString() == "\xC3\xA7\xC3\xA3o");    // ção
    CHECK(v->asArray()[1].asString() == "\xF0\x9F\x8E\xAC");      // clapper board emoji
    CHECK(v->asArray()[2].asString() == "a\tb\\c/d");
}

TEST_CASE("JSON rejects malformed documents", "[json]")
{
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "\"\\x\"", "tru", "[1] 2", "\"unterminated", "{\"a\":}",
                            "\"\\ud83c\""}) {
        INFO(bad);
        std::string error;
        CHECK_FALSE(ve::json::parse(bad, &error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("JSON accessors fall back on missing or mistyped values", "[json]")
{
    auto v = ve::json::parse(R"({"n": 3, "s": "x"})");
    REQUIRE(v);
    CHECK((*v)["missing"].isNull());
    CHECK((*v)["missing"]["deeper"].asInt(42) == 42);
    CHECK((*v)["s"].asInt(7) == 7);
    CHECK((*v)["n"].asString().empty());
    CHECK(v->contains("n"));
}
