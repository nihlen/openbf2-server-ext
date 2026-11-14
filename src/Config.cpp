#include "Config.h"

#include "AdminProtocol.h"

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

// A DNS name or IPv4 address: letters, digits, '-' and '.', 1..253 bytes.
bool validHostName(const std::string& s)
{
    if (s.empty() || s.size() > 253)
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return false;
    }
    return true;
}

bool isLoopbackHost(const std::string& host)
{
    return host == "localhost" || (parseIpv4(host) && host.compare(0, 4, "127.") == 0);
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
        else if (key == "stepMode")
        {
            if (value != "0" && value != "1")
            {
                diagnostics.push_back(std::string(where) + key + " must be 0 or 1, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.stepMode = value == "1";
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
        else if (key == "adminConnect")
        {
            const size_t colon = value.rfind(':');
            int port = 0;
            const std::string host = colon == std::string::npos ? std::string() : value.substr(0, colon);
            if (colon == std::string::npos || !validHostName(host) || !parseInt(value.substr(colon + 1), 1, 65535, port))
            {
                diagnostics.push_back(std::string(where) + "adminConnect must be <host>:<port>, got '" + value + "'");
                ok = false;
            }
            else if (port == 16567)
            {
                diagnostics.push_back(std::string(where) + "adminConnect must never use port 16567");
                ok = false;
            }
            else
            {
                out.adminEnabled = true;
                out.adminHost = host;
                out.adminPort = port;
            }
        }
        else if (key == "adminServerId")
        {
            bool printable = !value.empty() && value.size() <= 64;
            for (size_t i = 0; i < value.size() && printable; ++i)
                printable = value[i] > 0x20 && value[i] < 0x7F;
            if (!printable)
            {
                diagnostics.push_back(std::string(where) + "adminServerId must be 1..64 printable ASCII bytes without spaces");
                ok = false;
            }
            else
            {
                out.adminServerId = value;
            }
        }
        else if (key == "adminKey")
        {
            std::vector<uint8_t> raw;
            if (!admin::parseHex(value, raw) || raw.size() < 16 || raw.size() > 64)
            {
                diagnostics.push_back(std::string(where) + "adminKey must be 32..128 hex digits (16..64 bytes)");
                ok = false;
            }
            else
            {
                out.adminKey = raw;
            }
        }
        else if (key == "adminCertSha256")
        {
            std::vector<uint8_t> raw;
            if (!admin::parseHex(value, raw) || raw.size() != 32)
            {
                diagnostics.push_back(std::string(where) + "adminCertSha256 must be 64 hex digits (the SHA-256 of the SPKI)");
                ok = false;
            }
            else
            {
                out.adminCertSha256 = raw;
            }
        }
        else if (key == "adminTls" || key == "adminSendIdentity" || key == "adminAllowConsole" ||
                 key == "adminAllowObjects" || key == "adminAllowScripts")
        {
            if (value != "0" && value != "1")
            {
                diagnostics.push_back(std::string(where) + key + " must be 0 or 1, got '" + value + "'");
                ok = false;
            }
            else if (key == "adminTls")
                out.adminTls = value == "1";
            else if (key == "adminSendIdentity")
                out.adminSendIdentity = value == "1" ? 1 : 0;
            else if (key == "adminAllowConsole")
                out.adminAllowConsole = value == "1";
            else if (key == "adminAllowObjects")
                out.adminAllowObjects = value == "1";
            else
                out.adminAllowScripts = value == "1";
        }
        else if (key == "adminQueueBytes")
        {
            int bytes = 0;
            if (!parseInt(value, 64 * 1024, 64 * 1024 * 1024, bytes))
            {
                diagnostics.push_back(std::string(where) + "adminQueueBytes must be 65536..67108864, got '" + value + "'");
                ok = false;
            }
            else
            {
                out.adminQueueBytes = bytes;
            }
        }
        else
        {
            diagnostics.push_back(std::string(where) + "unknown key '" + key + "' ignored");
        }
    }

    // The admin channel's keys depend on each other.
    if (out.adminEnabled)
    {
        if (out.adminServerId.empty())
        {
            diagnostics.push_back("adminConnect needs adminServerId");
            ok = false;
        }
        if (out.adminKey.empty())
        {
            diagnostics.push_back("adminConnect needs adminKey");
            ok = false;
        }
        if (out.adminTls && out.adminCertSha256.empty())
        {
            diagnostics.push_back("adminTls 1 needs adminCertSha256 (the SHA-256 of WebAdmin's certificate's SPKI)");
            ok = false;
        }
        if (!out.adminTls && !isLoopbackHost(out.adminHost))
        {
            diagnostics.push_back("adminTls 0 is only allowed with a loopback adminConnect (127.x.x.x or localhost)");
            ok = false;
        }
    }
    else if (!out.adminServerId.empty() || !out.adminKey.empty() || !out.adminCertSha256.empty())
    {
        diagnostics.push_back("admin keys without adminConnect are ignored; the admin channel is off");
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
