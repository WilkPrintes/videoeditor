#include "core/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ve::json {

namespace {

const Value& nullValue()
{
    static const Value v;
    return v;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------------------------

bool Value::asBool(bool fallback) const
{
    const bool* b = std::get_if<bool>(&m_v);
    return b ? *b : fallback;
}

std::int64_t Value::asInt(std::int64_t fallback) const
{
    if (const std::int64_t* i = std::get_if<std::int64_t>(&m_v))
        return *i;
    if (const double* d = std::get_if<double>(&m_v))
        return std::int64_t(std::llround(*d));
    return fallback;
}

double Value::asDouble(double fallback) const
{
    if (const double* d = std::get_if<double>(&m_v))
        return *d;
    if (const std::int64_t* i = std::get_if<std::int64_t>(&m_v))
        return double(*i);
    return fallback;
}

const std::string& Value::asString() const
{
    static const std::string empty;
    const std::string* s = std::get_if<std::string>(&m_v);
    return s ? *s : empty;
}

const Value::Array& Value::asArray() const
{
    static const Array empty;
    const Array* a = std::get_if<Array>(&m_v);
    return a ? *a : empty;
}

const Value::Object& Value::asObject() const
{
    static const Object empty;
    const Object* o = std::get_if<Object>(&m_v);
    return o ? *o : empty;
}

const Value& Value::operator[](std::string_view key) const
{
    for (const auto& [k, v] : asObject()) {
        if (k == key)
            return v;
    }
    return nullValue();
}

bool Value::contains(std::string_view key) const
{
    for (const auto& [k, v] : asObject()) {
        if (k == key)
            return true;
    }
    return false;
}

Value& Value::set(std::string key, Value v)
{
    if (!std::holds_alternative<Object>(m_v))
        m_v = Object{};
    Object& o = std::get<Object>(m_v);
    for (auto& [k, existing] : o) {
        if (k == key) {
            existing = std::move(v);
            return existing;
        }
    }
    o.emplace_back(std::move(key), std::move(v));
    return o.back().second;
}

Value& Value::push(Value v)
{
    if (!std::holds_alternative<Array>(m_v))
        m_v = Array{};
    Array& a = std::get<Array>(m_v);
    a.push_back(std::move(v));
    return a.back();
}

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------

namespace {

void writeString(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c); // UTF-8 passes through
            }
        }
    }
    out += '"';
}

std::string formatDouble(double d)
{
    if (!std::isfinite(d))
        return "null";
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(17) << d;
    std::string text = s.str();
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0"; // keep it a double when read back
    return text;
}

void write(std::string& out, const Value& v, int indent, int depth)
{
    auto newline = [&](int level) {
        if (indent <= 0)
            return;
        out += '\n';
        out.append(std::size_t(level * indent), ' ');
    };
    switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Int: out += std::to_string(v.asInt()); break;
    case Value::Type::Double: out += formatDouble(v.asDouble()); break;
    case Value::Type::String: writeString(out, v.asString()); break;
    case Value::Type::Array: {
        const auto& a = v.asArray();
        if (a.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i)
                out += ',';
            newline(depth + 1);
            write(out, a[i], indent, depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    }
    case Value::Type::Object: {
        const auto& o = v.asObject();
        if (o.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < o.size(); ++i) {
            if (i)
                out += ',';
            newline(depth + 1);
            writeString(out, o[i].first);
            out += indent > 0 ? ": " : ":";
            write(out, o[i].second, indent, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::optional<Value> run(std::string* error)
    {
        Value v;
        skipSpace();
        if (!parseValue(v, 0) || (skipSpace(), m_pos != m_text.size())) {
            if (error)
                *error = (m_error.empty() ? std::string("unexpected data") : m_error) + " at offset "
                    + std::to_string(m_pos);
            return std::nullopt;
        }
        return v;
    }

private:
    bool fail(const char* what)
    {
        if (m_error.empty())
            m_error = what;
        return false;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size()
               && (m_text[m_pos] == ' ' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r' || m_text[m_pos] == '\t'))
            ++m_pos;
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word)
            return fail("invalid literal");
        m_pos += word.size();
        return true;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > 256)
            return fail("nesting too deep");
        if (m_pos >= m_text.size())
            return fail("unexpected end");
        switch (m_text[m_pos]) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string s;
            if (!parseString(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        case 't': out = Value(true); return literal("true");
        case 'f': out = Value(false); return literal("false");
        case 'n': out = Value(); return literal("null");
        default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth)
    {
        ++m_pos; // {
        out = Value::object();
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !parseString(key))
                return fail("expected a key");
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                return fail("expected ':'");
            ++m_pos;
            skipSpace();
            Value v;
            if (!parseValue(v, depth + 1))
                return false;
            out.set(std::move(key), std::move(v));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value& out, int depth)
    {
        ++m_pos; // [
        out = Value::array();
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        for (;;) {
            skipSpace();
            Value v;
            if (!parseValue(v, depth + 1))
                return false;
            out.push(std::move(v));
            skipSpace();
            if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                ++m_pos;
                continue;
            }
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseHex4(unsigned& code)
    {
        if (m_pos + 4 > m_text.size())
            return fail("short \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') code |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= unsigned(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) {
            s += char(cp);
        } else if (cp < 0x800) {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out)
    {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_pos >= m_text.size())
                break;
            const char e = m_text[m_pos++];
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
                unsigned cp = 0;
                if (!parseHex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) { // surrogate pair
                    unsigned low = 0;
                    if (m_text.substr(m_pos, 2) != "\\u")
                        return fail("lone surrogate");
                    m_pos += 2;
                    if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value& out)
    {
        const std::size_t start = m_pos;
        bool isDouble = false;
        if (m_pos < m_text.size() && m_text[m_pos] == '-')
            ++m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c >= '0' && c <= '9') {
                ++m_pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isDouble = true;
                ++m_pos;
            } else {
                break;
            }
        }
        const std::string_view token = m_text.substr(start, m_pos - start);
        if (token.empty() || token == "-")
            return fail("invalid value");
        if (!isDouble) {
            std::int64_t i = 0;
            const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), i);
            if (ec == std::errc() && ptr == token.data() + token.size()) {
                out = Value(i);
                return true;
            }
        }
        std::istringstream s{std::string(token)};
        s.imbue(std::locale::classic());
        double d = 0.0;
        s >> d;
        if (s.fail() || !s.eof())
            return fail("invalid number");
        out = Value(d);
        return true;
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
    std::string m_error;
};

} // namespace

std::string serialize(const Value& value, int indent)
{
    std::string out;
    write(out, value, indent, 0);
    if (indent > 0)
        out += '\n';
    return out;
}

std::optional<Value> parse(std::string_view text, std::string* error)
{
    return Parser(text).run(error);
}

} // namespace ve::json
