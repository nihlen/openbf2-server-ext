// serverext.cfg, docs/protocol.md "Loading". Engine-free.

#ifndef SERVEREXT_CONFIG_H
#define SERVEREXT_CONFIG_H

#include <string>
#include <vector>

namespace serverext {

struct Config
{
    std::string listenHost;   // IPv4 dotted quad
    int listenPort;
    int watchdogTicks;
    float enterRange;            // metres, v1
    int spawnFallbackSeconds;    // v1
    int spawnBlockedSeconds;     // v1
    bool allowRoundRestart;      // v1
    int maxConnections;          // v2: sidecar connections open at once
    bool allowTeleport;          // v2: Teleport is refused unless set
    float projectileRange;       // v2: metres around an own bot within which projectiles are reported

    Config()
        : listenHost("127.0.0.1"), listenPort(16700), watchdogTicks(6), enterRange(30.0f),
          spawnFallbackSeconds(5), spawnBlockedSeconds(15), allowRoundRestart(false), maxConnections(4),
          allowTeleport(false), projectileRange(1000.0f) {}
};

// Parses the file's text. One "key value" or "key = value" per line; '#' and "//" start a comment.
// Keys are case-sensitive, as in the contract. Unknown keys are warnings. A malformed or out-of-range
// value is an error and leaves that setting at its default. Port 16567 is always an error.
// @return True when there were no errors. Messages are appended to diagnostics either way.
bool parseConfig(const std::string& text, Config& out, std::vector<std::string>& diagnostics);

// Reads path into text. @return False when the file does not exist or cannot be read.
bool readFile(const std::string& path, std::string& text);

} // namespace serverext

#endif // SERVEREXT_CONFIG_H
