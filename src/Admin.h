// The engine side of the admin channel (docs/admin-channel.md). It runs on the server's main thread: the bot channel's per-tick
// handler calls tick(), and the engine calls its listeners (script events through R4, damage
// through R5, projectile launches, game status and level unload). The socket work is on the admin
// I/O thread (AdminIo); the two meet only in AdminLink.

#ifndef SERVEREXT_ADMIN_H
#define SERVEREXT_ADMIN_H

#include <cstdint>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
class ClassManager;
namespace world { class IObject; }
} // namespace hfe
} // namespace dice

namespace serverext {

struct Config;

// What the admin channel needs from the bot channel's module.
class AdminHost
{
public:
    // True when playerId is a bot created by the bot channel; connection receives its connection.
    virtual bool botChannelBot(int playerId, uint32_t* connection) = 0;
    // The live vehicle roots of the bot channel's vehicle set (docs/protocol.md, VehicleList).
    virtual void vehicleRoots(std::vector<dice::hfe::world::IObject*>& out) = 0;

protected:
    ~AdminHost() {}
};

class AdminChannel
{
public:
    virtual ~AdminChannel() {}
    // Once per simulation tick, from the bot channel's tick handler, with its tick number.
    virtual void tick(unsigned long tick) = 0;
    // Bot-channel bot notifications: kind is admin::BotKind.
    virtual void onBot(uint8_t kind, int playerId, int team, uint32_t connection) = 0;
    // One line for the module's load log.
    virtual std::string describe() const = 0;
};

typedef void (*AdminLogFn)(const char* line);

// Creates the admin channel from serverext.cfg's admin keys and starts its I/O thread.
// @return Null with error set when a required engine singleton or hook is missing or the thread
//         cannot start.
AdminChannel* startAdminChannel(const Config& config, dice::hfe::ClassManager* classManager, AdminHost* host,
                                AdminLogFn log, std::string& error);

} // namespace serverext

#endif // SERVEREXT_ADMIN_H
