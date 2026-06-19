// Minimal INI reader + line-preserving section writer (case-insensitive keys).
#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ini {

inline std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

class Ini
{
public:
    // section(lower) -> { key(lower) -> raw value }
    std::map<std::string, std::map<std::string, std::string>> data;

    bool load(const std::wstring& path)
    {
        std::ifstream f(path);
        if (!f) return false;
        std::string line, section;
        while (std::getline(f, line))
        {
            std::string t = trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t.front() == '[' && t.back() == ']')
            {
                section = lower(trim(t.substr(1, t.size() - 2)));
                continue;
            }
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trim(t.substr(0, eq)));
            std::string val = trim(t.substr(eq + 1));
            // strip inline comment after value (only when preceded by whitespace)
            for (size_t i = 1; i < val.size(); ++i)
            {
                if ((val[i] == ';' || val[i] == '#') && (val[i - 1] == ' ' || val[i - 1] == '\t'))
                {
                    val = trim(val.substr(0, i));
                    break;
                }
            }
            data[section][key] = val;
        }
        return true;
    }

    bool has(const std::string& section, const std::string& key) const
    {
        auto s = data.find(lower(section));
        if (s == data.end()) return false;
        return s->second.count(lower(key)) > 0;
    }

    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const
    {
        auto s = data.find(lower(section));
        if (s == data.end()) return def;
        auto k = s->second.find(lower(key));
        return k == s->second.end() ? def : k->second;
    }

    bool get_bool(const std::string& section, const std::string& key, bool def) const
    {
        if (!has(section, key)) return def;
        std::string v = lower(get(section, key));
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
};

inline std::string key_original_case(const std::string& line, size_t eq)
{
    // keep the original key text (trimmed) so casing in the file is preserved
    return trim(line.substr(0, eq));
}

// Update keys within [section], preserving the rest of the file; creates them if absent.
inline bool write_section(const std::wstring& path, const std::string& section,
                          const std::vector<std::pair<std::string, std::string>>& kv)
{
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::map<std::string, bool> written;
    for (auto& p : kv) written[lower(p.first)] = false;

    auto find_val = [&](const std::string& keyLower) -> const std::string* {
        for (auto& p : kv)
            if (lower(p.first) == keyLower) return &p.second;
        return nullptr;
    };

    std::vector<std::string> out;
    std::string cur;
    bool inTarget = false;
    int targetEndIdx = -1;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[' && t.back() == ']')
        {
            if (inTarget) targetEndIdx = (int)out.size(); // end of target section
            cur = lower(trim(t.substr(1, t.size() - 2)));
            inTarget = (cur == lower(section));
            out.push_back(lines[i]);
            continue;
        }
        if (inTarget && !t.empty() && t[0] != ';' && t[0] != '#')
        {
            auto eq = t.find('=');
            if (eq != std::string::npos)
            {
                std::string key = lower(trim(t.substr(0, eq)));
                if (const std::string* v = find_val(key))
                {
                    out.push_back(key_original_case(t, eq) + " = " + *v);
                    written[key] = true;
                    continue;
                }
            }
        }
        out.push_back(lines[i]);
    }
    if (inTarget) targetEndIdx = (int)out.size();

    // Append any keys not yet written.
    std::vector<std::string> pending;
    for (auto& p : kv)
        if (!written[lower(p.first)]) pending.push_back(p.first + " = " + p.second);

    if (!pending.empty())
    {
        if (targetEndIdx < 0)
        {
            // section didn't exist: append it
            if (!out.empty() && !trim(out.back()).empty()) out.push_back("");
            out.push_back("[" + section + "]");
            for (auto& l : pending) out.push_back(l);
        }
        else
        {
            out.insert(out.begin() + targetEndIdx, pending.begin(), pending.end());
        }
    }

    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    if (!f) return false;
    for (auto& l : out) f << l << "\r\n";
    return true;
}

} // namespace ini
