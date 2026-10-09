/*
 * Asagi Studio — a small JSON reader and writer helpers (header only)
 *
 * Enough for manifests, settings files and the bridge protocol: objects, arrays, strings (with
 * \uXXXX escapes and surrogate pairs), numbers, booleans and null. Nesting depth and input size
 * are bounded so untrusted input cannot exhaust the stack.
 *
 * SPDX-License-Identifier: ISC
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace asagi {
namespace json {

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    bool isNull() const noexcept { return type == Type::Null; }
    bool isBool() const noexcept { return type == Type::Bool; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isString() const noexcept { return type == Type::String; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isObject() const noexcept { return type == Type::Object; }

    // Member lookup; returns nullptr when this is not an object or the key is missing.
    const Value* find(const char* key) const noexcept
    {
        if (type != Type::Object)
            return nullptr;
        for (const auto& kv : o)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    const Value& operator[](const char* key) const noexcept
    {
        const Value* v = find(key);
        return v ? *v : null();
    }

    const Value& operator[](const size_t i) const noexcept
    {
        return type == Type::Array && i < a.size() ? a[i] : null();
    }

    const Value& operator[](const int i) const noexcept { return i < 0 ? null() : (*this)[size_t(i)]; }

    size_t size() const noexcept { return type == Type::Array ? a.size() : type == Type::Object ? o.size() : 0; }

    std::string str(const std::string& fallback = std::string()) const { return type == Type::String ? s : fallback; }
    double num(const double fallback = 0.0) const noexcept { return type == Type::Number ? n : fallback; }
    bool boolean(const bool fallback = false) const noexcept { return type == Type::Bool ? b : fallback; }

    // Integer value; non-integral or out-of-range numbers give the fallback.
    long long integer(const long long fallback = 0) const noexcept
    {
        if (type != Type::Number || ! std::isfinite(n) || n < -9.0e15 || n > 9.0e15 || std::floor(n) != n)
            return fallback;
        return static_cast<long long>(n);
    }

    static const Value& null() noexcept
    {
        static const Value v;
        return v;
    }
};

namespace detail {

class Parser
{
public:
    Parser(const char* p, const char* end) : fP(p), fEnd(end) {}

    bool parseDocument(Value& out, std::string* error)
    {
        skipSpace();
        if (! parseValue(out, 0))
            return fail(error);
        skipSpace();
        if (fP != fEnd)
        {
            fError = "trailing characters";
            return fail(error);
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(std::string* error)
    {
        if (error)
            *error = fError.empty() ? "invalid JSON" : fError;
        return false;
    }

    void skipSpace() noexcept
    {
        while (fP != fEnd && (*fP == ' ' || *fP == '\t' || *fP == '\n' || *fP == '\r'))
            ++fP;
    }

    bool literal(const char* word)
    {
        const size_t n = std::strlen(word);
        if (size_t(fEnd - fP) < n || std::memcmp(fP, word, n) != 0)
        {
            fError = "unexpected token";
            return false;
        }
        fP += n;
        return true;
    }

    bool parseValue(Value& v, const int depth)
    {
        if (depth > kMaxDepth)
        {
            fError = "nesting too deep";
            return false;
        }
        if (fP == fEnd)
        {
            fError = "unexpected end";
            return false;
        }
        switch (*fP)
        {
        case '{': return parseObject(v, depth);
        case '[': return parseArray(v, depth);
        case '"': v.type = Value::Type::String; return parseString(v.s);
        case 't': v.type = Value::Type::Bool; v.b = true; return literal("true");
        case 'f': v.type = Value::Type::Bool; v.b = false; return literal("false");
        case 'n': v.type = Value::Type::Null; return literal("null");
        default:  return parseNumber(v);
        }
    }

    bool parseObject(Value& v, const int depth)
    {
        v.type = Value::Type::Object;
        ++fP;  // {
        skipSpace();
        if (fP != fEnd && *fP == '}')
        {
            ++fP;
            return true;
        }
        for (;;)
        {
            skipSpace();
            if (fP == fEnd || *fP != '"')
            {
                fError = "expected a key";
                return false;
            }
            std::string key;
            if (! parseString(key))
                return false;
            skipSpace();
            if (fP == fEnd || *fP != ':')
            {
                fError = "expected ':'";
                return false;
            }
            ++fP;
            skipSpace();
            v.o.emplace_back(std::move(key), Value());
            if (! parseValue(v.o.back().second, depth + 1))
                return false;
            skipSpace();
            if (fP != fEnd && *fP == ',')
            {
                ++fP;
                continue;
            }
            if (fP != fEnd && *fP == '}')
            {
                ++fP;
                return true;
            }
            fError = "expected ',' or '}'";
            return false;
        }
    }

    bool parseArray(Value& v, const int depth)
    {
        v.type = Value::Type::Array;
        ++fP;  // [
        skipSpace();
        if (fP != fEnd && *fP == ']')
        {
            ++fP;
            return true;
        }
        for (;;)
        {
            skipSpace();
            v.a.emplace_back();
            if (! parseValue(v.a.back(), depth + 1))
                return false;
            skipSpace();
            if (fP != fEnd && *fP == ',')
            {
                ++fP;
                continue;
            }
            if (fP != fEnd && *fP == ']')
            {
                ++fP;
                return true;
            }
            fError = "expected ',' or ']'";
            return false;
        }
    }

    static int hex(const char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(uint32_t& out)
    {
        if (fEnd - fP < 4)
        {
            fError = "bad \\u escape";
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const int h = hex(fP[i]);
            if (h < 0)
            {
                fError = "bad \\u escape";
                return false;
            }
            out = (out << 4) | uint32_t(h);
        }
        fP += 4;
        return true;
    }

    static void appendUtf8(std::string& s, const uint32_t cp)
    {
        if (cp < 0x80)
            s += char(cp);
        else if (cp < 0x800)
        {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
        else
        {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out)
    {
        ++fP;  // opening quote
        out.clear();
        while (fP != fEnd)
        {
            const char c = *fP++;
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
            {
                fError = "control character in string";
                return false;
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (fP == fEnd)
                break;
            const char e = *fP++;
            switch (e)
            {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
            {
                uint32_t cp;
                if (! hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    uint32_t lo;
                    if (fEnd - fP >= 6 && fP[0] == '\\' && fP[1] == 'u')
                    {
                        fP += 2;
                        if (! hex4(lo))
                            return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            cp = 0xFFFD;
                    }
                    else
                        cp = 0xFFFD;
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    cp = 0xFFFD;
                appendUtf8(out, cp);
                break;
            }
            default:
                fError = "bad escape";
                return false;
            }
        }
        fError = "unterminated string";
        return false;
    }

    bool parseNumber(Value& v)
    {
        const char* start = fP;
        if (fP != fEnd && *fP == '-')
            ++fP;
        if (fP == fEnd || ! (*fP >= '0' && *fP <= '9'))
        {
            fError = "unexpected token";
            return false;
        }
        while (fP != fEnd && ((*fP >= '0' && *fP <= '9') || *fP == '.' || *fP == 'e' || *fP == 'E' || *fP == '+' || *fP == '-'))
            ++fP;
        const std::string text(start, fP);
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0' || ! std::isfinite(d))
        {
            fError = "bad number";
            return false;
        }
        v.type = Value::Type::Number;
        v.n = d;
        return true;
    }

    const char* fP;
    const char* fEnd;
    std::string fError;
};

} // namespace detail

// Parses a whole document. Inputs above maxBytes are rejected.
inline bool parse(const std::string& text, Value& out, std::string* error = nullptr, const size_t maxBytes = 4u << 20)
{
    out = Value();
    if (text.size() > maxBytes)
    {
        if (error)
            *error = "document too large";
        return false;
    }
    const char* p = text.data();
    const char* end = p + text.size();
    // tolerate a UTF-8 byte order mark (files saved by Notepad)
    if (text.size() >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB
        && static_cast<unsigned char>(p[2]) == 0xBF)
        p += 3;
    detail::Parser parser(p, end);
    return parser.parseDocument(out, error);
}

// Appends s as a quoted JSON string.
inline void quote(std::string& out, const std::string& s)
{
    out += '"';
    for (const unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            }
            else
                out += char(c);
        }
    }
    out += '"';
}

inline std::string quoted(const std::string& s)
{
    std::string out;
    quote(out, s);
    return out;
}

} // namespace json
} // namespace asagi
