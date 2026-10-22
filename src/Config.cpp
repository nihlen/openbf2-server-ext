#include "Config.h"

#include <cstdio>
#include <cstdlib>

namespace serverext {

namespace {

std::string trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool parseInt(const std::string& s, long lo, long hi, int& out)
{
    if (s.empty() || s.size() > 10)
        return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9')
            return false;
    const long v = std::strtol(s.c_str(), 0, 10);
    if (v < lo || v > hi)
        return false;
    out = static_cast<int>(v);
    return true;
}

// A plain decimal number: digits with at most one '.', no sign or exponent.
bool parseFloat(const std::string& s, double lo, double hi, float& out)
{
    if (s.empty() || s.size() > 12)
        return false;
    int dots = 0, digits = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '.')
            ++dots;
        else if (s[i] >= '0' && s[i] <= '9')
            ++digits;
        else
            return false;
    }
    if (dots > 1 || digits == 0)
        return false;
    const double v = std::strtod(s.c_str(), 0);
    if (!(v >= lo && v <= hi))
        return false;
    out = static_cast<float>(v);
    return true;
}

bool parseIpv4(const std::string& s)
{
    int parts = 0;
    size_t start = 0;
    while (true)
    {
        const size_t dot = s.find('.', start);
        const std::string part = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        int v;
        if (!parseInt(part, 0, 255, v) || part.size() > 3)
            return false;
        ++parts;
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return parts == 4;
}

} // namespace

bool parseConfig(const std::string& text, Config& out, std::vector<std::string>& diagnostics)
{
    bool ok = true;
    size_t lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        size_t comment = line.find('#');
        const size_t slashes = line.find("//");
        if (slashes != std::string::npos && (comment == std::string::npos || slashes < comment))
            comment = slashes;
        if (comment != std::string::npos)
            line.erase(comment);
        line = trim(line);
        if (line.empty())
            continue;

        size_t sep = line.find_first_of(" \t=");
        const std::string key = line.substr(0, sep);
        std::string value = sep == std::string::npos ? std::string() : trim(line.substr(sep));
        if (!value.empty() && value[0] == '=')
            value = trim(value.substr(1));

        char where[32];
        std::snprintf(where, sizeof(where), "line %u: ", static_cast<unsigned>(lineNo));

        if (key == "botListen")
        {
            const size_t colon = value.rfind(':');
            int port = 0;
            const std::string host = colon == std::string::npos ? std::string() : value.substr(0, colon);
            if (colon == std::string::npos || !parseIpv4(host) ||
                !parseInt(value.substr(colon + 1), 1, 65535, port))
            {
                diagnostics.push_back(std::string(where) + "botListen must be <ipv4>:<port>, got '" + value + "'");
                ok = false;
            }
            else if (port == 16567)
            {
                diagnostics.push_back(std::string(where) + "botListen must never use port 16567");
                ok = false;
            }
            else
            {
                out.listenHost = host;
                out.listenPort = port;
            }
        }
        else if (key == "watchdogTicks")
        {
            int ticks = 0;
            if (!parseInt(value, 1, 3000, ticks))
            {
                diagnostics.push_back(std::string(where) + "watchdogTicks must be 1..3000, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.watchdogTicks = ticks;
            }
        }
        else if (key == "enterRange")
        {
            float range = 0;
            if (!parseFloat(value, 1.0, 1000.0, range))
            {
                diagnostics.push_back(std::string(where) + "enterRange must be 1..1000 metres, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.enterRange = range;
            }
        }
        else if (key == "spawnFallbackSeconds" || key == "spawnBlockedSeconds")
        {
            int seconds = 0;
            if (!parseInt(value, 1, 3600, seconds))
            {
                diagnostics.push_back(std::string(where) + key + " must be 1..3600, got '" + value + "'");
                ok = false;
            }
            else if (key == "spawnFallbackSeconds")
            {
                out.spawnFallbackSeconds = seconds;
            }
            else
            {
                out.spawnBlockedSeconds = seconds;
            }
        }
        else if (key == "allowRoundRestart" || key == "allowTeleport")
        {
            if (value != "0" && value != "1")
            {
                diagnostics.push_back(std::string(where) + key + " must be 0 or 1, got '" + value + "'");
                ok = false;
            }
            else if (key == "allowRoundRestart")
            {
                out.allowRoundRestart = value == "1";
            }
            else
            {
                out.allowTeleport = value == "1";
            }
        }
        else if (key == "maxConnections")
        {
            int n = 0;
            if (!parseInt(value, 1, 8, n))
            {
                diagnostics.push_back(std::string(where) + "maxConnections must be 1..8, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.maxConnections = n;
            }
        }
        else if (key == "projectileRange")
        {
            float range = 0;
            if (!parseFloat(value, 10.0, 5000.0, range))
            {
                diagnostics.push_back(std::string(where) + "projectileRange must be 10..5000 metres, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.projectileRange = range;
            }
        }
        else
        {
            diagnostics.push_back(std::string(where) + "unknown key '" + key + "' ignored");
        }
    }
    return ok;
}

bool readFile(const std::string& path, std::string& text)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    text.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 1024 * 1024)
        text.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

} // namespace serverext
