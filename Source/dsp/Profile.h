#pragma once
// Studio Engine - reference-match profile loading/saving (JSON).
//
// A profile is produced by tools/reference_match/studio_match.py:
//   {
//     "name": "My reference",
//     "params": { "comp_ratio": 3.4, "match_1k": -1.2, ... },
//     "analysis": { ... informational only ... }
//   }
// Only "name" and "params" are read; unknown keys are ignored.
#include "Params.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace se
{
/** Very small JSON reader (objects, arrays, strings, numbers, bools, null). */
struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0.0;
    bool boolean = false;
    std::string str;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* get (const std::string& key) const
    {
        auto it = object.find (key);
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonParser
{
public:
    explicit JsonParser (const std::string& text) : s (text) {}

    bool parse (JsonValue& out)
    {
        try
        {
            out = value();
            skipWs();
            return pos == s.size();
        }
        catch (...)
        {
            return false;
        }
    }

private:
    const std::string& s;
    size_t pos = 0;

    [[noreturn]] void fail() { throw 0; }
    void skipWs()
    {
        while (pos < s.size() && std::isspace ((unsigned char) s[pos]))
            ++pos;
    }
    char peek()
    {
        skipWs();
        if (pos >= s.size())
            fail();
        return s[pos];
    }
    void expect (char c)
    {
        if (peek() != c)
            fail();
        ++pos;
    }

    JsonValue value()
    {
        const char c = peek();
        if (c == '{') return objectValue();
        if (c == '[') return arrayValue();
        if (c == '"') { JsonValue v; v.type = JsonValue::Type::String; v.str = stringValue(); return v; }
        if (s.compare (pos, 4, "true") == 0) { pos += 4; JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = true; return v; }
        if (s.compare (pos, 5, "false") == 0) { pos += 5; JsonValue v; v.type = JsonValue::Type::Bool; return v; }
        if (s.compare (pos, 4, "null") == 0) { pos += 4; return {}; }
        return numberValue();
    }

    JsonValue objectValue()
    {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        expect ('{');
        if (peek() == '}') { ++pos; return v; }
        for (;;)
        {
            if (peek() != '"') fail();
            auto key = stringValue();
            expect (':');
            v.object[key] = value();
            if (peek() == ',') { ++pos; continue; }
            expect ('}');
            return v;
        }
    }

    JsonValue arrayValue()
    {
        JsonValue v;
        v.type = JsonValue::Type::Array;
        expect ('[');
        if (peek() == ']') { ++pos; return v; }
        for (;;)
        {
            v.array.push_back (value());
            if (peek() == ',') { ++pos; continue; }
            expect (']');
            return v;
        }
    }

    std::string stringValue()
    {
        expect ('"');
        std::string out;
        while (pos < s.size() && s[pos] != '"')
        {
            char c = s[pos++];
            if (c == '\\' && pos < s.size())
            {
                const char e = s[pos++];
                switch (e)
                {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': pos = std::min (s.size(), pos + 4); out += '?'; break;
                    default: out += e; break;
                }
            }
            else
                out += c;
        }
        expect ('"');
        return out;
    }

    JsonValue numberValue()
    {
        skipWs();
        const char* begin = s.c_str() + pos;
        char* end = nullptr;
        const double d = std::strtod (begin, &end);
        if (end == begin)
            fail();
        pos += (size_t) (end - begin);
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.number = d;
        return v;
    }
};

struct Profile
{
    std::string name;
    std::vector<std::pair<int, float>> values; // (param index, value)
};

/** Parse profile JSON text. Returns false on malformed input. */
inline bool parseProfile (const std::string& text, Profile& out, std::string* error = nullptr)
{
    JsonValue root;
    if (! JsonParser (text).parse (root) || root.type != JsonValue::Type::Object)
    {
        if (error) *error = "not valid JSON";
        return false;
    }
    out = {};
    if (auto* n = root.get ("name"); n && n->type == JsonValue::Type::String)
        out.name = n->str;

    const JsonValue* params = root.get ("params");
    if (params == nullptr || params->type != JsonValue::Type::Object)
    {
        if (error) *error = "missing \"params\" object";
        return false;
    }
    for (const auto& [key, v] : params->object)
    {
        const int idx = findParam (key);
        if (idx < 0)
            continue;
        double d = 0.0;
        if (v.type == JsonValue::Type::Number) d = v.number;
        else if (v.type == JsonValue::Type::Bool) d = v.boolean ? 1.0 : 0.0;
        else continue;
        const auto& info = paramTable()[(size_t) idx];
        d = std::fmin (std::fmax (d, info.minValue), info.maxValue);
        out.values.emplace_back (idx, (float) d);
    }
    return true;
}

inline bool loadProfileFile (const std::string& path, Profile& out, std::string* error = nullptr)
{
    std::ifstream f (path);
    if (! f)
    {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseProfile (ss.str(), out, error);
}

/** Serialise parameter values as a profile JSON string. */
template <typename Values>
std::string profileToJson (const std::string& name, const Values& values)
{
    std::ostringstream o;
    o << "{\n  \"name\": \"";
    for (char c : name)
        o << (c == '"' || c == '\\' ? std::string ("\\") + c : std::string (1, c));
    o << "\",\n  \"params\": {\n";
    for (int i = 0; i < kNumParams; ++i)
        o << "    \"" << paramTable()[(size_t) i].id << "\": " << values[(size_t) i]
          << (i + 1 < kNumParams ? ",\n" : "\n");
    o << "  }\n}\n";
    return o.str();
}

} // namespace se
