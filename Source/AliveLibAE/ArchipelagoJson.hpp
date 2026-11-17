#pragma once

// Tiny helpers for reading the flat JSON objects the Archipelago client writes. Header-only and free of engine
// types so it can be compiled and tested on its own.

#include <cstdlib>
#include <string>
#include <vector>

namespace Archipelago {
namespace Json {

// Splits text into its top-level JSON objects, ignoring whatever is between them (newlines, a missing newline, a byte
// order mark). Objects are found by counting braces, so two objects glued onto one line, or a last line with no newline,
// are both read correctly. A trailing object that is not complete yet is left alone.
//
// Returns how many characters were used up; the caller should start next time from there.
inline size_t ExtractObjects(const std::string& text, std::vector<std::string>& out)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t start = 0;
    size_t used = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        const char c = text[i];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                inString = false;
            }
        }
        else if (depth > 0 && c == '"')
        {
            inString = true;
        }
        else if (c == '{')
        {
            if (depth == 0)
            {
                start = i;
            }
            depth++;
        }
        else if (c == '}' && depth > 0)
        {
            depth--;
            if (depth == 0)
            {
                out.push_back(text.substr(start, i - start + 1));
                used = i + 1;
            }
        }
    }
    // Nothing half-written at the end: everything has been looked at.
    return depth == 0 ? text.size() : used;
}

inline bool FindValue(const std::string& object, const char* key, size_t& pos)
{
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = object.find(needle);
    if (at == std::string::npos)
    {
        return false;
    }
    pos = at + needle.size();
    while (pos < object.size() && object[pos] == ' ')
    {
        pos++;
    }
    return pos < object.size();
}

inline long long Int(const std::string& object, const char* key, long long fallback)
{
    size_t pos = 0;
    return FindValue(object, key, pos) ? std::strtoll(object.c_str() + pos, nullptr, 10) : fallback;
}

inline std::string String(const std::string& object, const char* key)
{
    size_t pos = 0;
    if (!FindValue(object, key, pos) || object[pos] != '"')
    {
        return {};
    }
    const size_t end = object.find('"', pos + 1);
    return end == std::string::npos ? std::string() : object.substr(pos + 1, end - pos - 1);
}

} // namespace Json
} // namespace Archipelago
