#pragma once

// Minimal JSON for project files and golden test states: no dependencies, locale independent,
// integers kept exact as int64 (flicks do not survive a round trip through double), and object keys
// kept in insertion order so files diff cleanly.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ve::json {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_v(b) {}
    Value(int i) : m_v(std::int64_t(i)) {}
    Value(std::int64_t i) : m_v(i) {}
    Value(double d) : m_v(d) {}
    Value(const char* s) : m_v(std::string(s)) {}
    Value(std::string s) : m_v(std::move(s)) {}
    Value(Array a) : m_v(std::move(a)) {}
    Value(Object o) : m_v(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const { return Type(m_v.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isObject() const { return type() == Type::Object; }
    bool isArray() const { return type() == Type::Array; }

    bool asBool(bool fallback = false) const;
    std::int64_t asInt(std::int64_t fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    const std::string& asString() const; // empty when not a string
    const Array& asArray() const;        // empty when not an array
    const Object& asObject() const;      // empty when not an object

    // Object access: a null value when the key is missing.
    const Value& operator[](std::string_view key) const;
    bool contains(std::string_view key) const;
    // Inserts or replaces (turns a null value into an object).
    Value& set(std::string key, Value v);
    // Appends (turns a null value into an array).
    Value& push(Value v);

    bool operator==(const Value& other) const { return m_v == other.m_v; }

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> m_v;
};

std::string serialize(const Value& value, int indent = 2);
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

} // namespace ve::json
