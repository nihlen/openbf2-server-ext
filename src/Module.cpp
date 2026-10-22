// libserverext: the engine side of the bot channel, docs/protocol.md v2 (byte layouts in
// docs/findings/module-v2.md).
//
// Everything in this file runs on the server's main thread: inside the EventManager category-7
// event that the module re-posts every tick (Q1), the ECHud unload notification, the ECNetwork chat
// event, and the engine's damage, projectile and spot observers (IServerExtension v2). Socket work
// is on the I/O thread (IoThread_posix.cpp); the two meet only in Channel.
//
// Engine access is limited to typed virtual calls and inline header code, because bf2 exports no
// symbols to modules.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Core/ClassManager.h"
#include "Core/EventManager.h"
#include "Core/Ids.h"
#include "Core/System.h"
#include "Game/Common/StringManager.h"
#include "Game/IEventListener.h"
#include "Game/IGame.h"
#include "Game/IGameServer.h"
#include "Game/Objects/ICameraObject.h"
#include "Game/GameServer/IServerExtension.h"
#include "Game/Objects/GameLogicComp/IArmorGLComp.h"
#include "Game/Objects/IObject.h"
#include "Game/Objects/IObjectTemplate.h"
#include "Game/Objects/IPlayer.h"
#include "Game/Objects/IPlayerControlObjectTemplate.h"
#include "Game/Objects/IWeaponObject.h"
#include "IO/Input/PlayerInput.h"
#include "IO/Network/INetworkable.h"
#include "Physics/IPhysicsNode.h"
#include "Scene/Object/ObjectManager.h"
#include "Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#include "Scene/Object/PlayerManager.h"

#include "Channel.h"
#include "Config.h"
#include "IoThread.h"
#include "Neutral.h"
#include "Protocol.h"
#include "SpawnPlan.h"

using namespace dice::hfe;
using namespace dice::hfe::world;
using namespace serverext;

namespace {

const EventCategory kTickCategory = static_cast<EventCategory>(7);   // [CHOSEN] Q1/Q8
const unsigned int kTickEventId = 1;
const unsigned int kUnloadHudEventId = 3;   // GameServer::unload posts (ECHud, 3) first
const unsigned int kChatEventId = 11;       // StringReceivedEvent on ECNetwork (StringManager.h)
const float kTickRate = 30.0f;
const int kDefaultKit = 0;
const unsigned int kStatsPeriodTicks = 1800;   // one stats line per minute
const unsigned int kGroupRetryTicks = 10;      // re-set a cleared target spawn group this often
const unsigned int kResolveRetryTicks = 30;    // retry a team without a usable spawn group once a second
const unsigned int kVehicleListPeriod = 30;     // protocol.md v1: once a second
const size_t kVehicleScanPerTick = 500;         // registered objects examined per tick
const unsigned int kRestartMinTicks = 60 * 30;  // protocol.md v1: at most one RestartRound per 60 s
const unsigned int kSpawnBlockedRepeatTicks = 10 * 30;
const int kMaxSpawnGroups = 64;
const int kGameStatusPlaying = 1;               // the only status in which dead players respawn
const unsigned long kSpottedTicks = 12 * 30;    // the client's minimap marker lifetime (Bf2MenuMapNode.cpp:842)
const unsigned long kSpotMinTicks = 30;         // at most one Spot per bot per second
const size_t kMaxTrackedProjectiles = 256;
const unsigned int kCachedObjectFlag = 0x20;    // ObjectCache: a disabled, recycled object
const int kCategoryHelicopter = 3;              // VCHelicopter (ModuleWorldObjTemplGame.cpp); every heli's vehicleCategory
const uint32_t kNoNetIdBase = 0x10000;          // ProjectileList id without a networkable id

// protocol.md v1 "Buttons": SetInput bit n presses PlayerInput slot kButtonSlots[n] for one tick.
// The table and its evidence are in docs/findings/module-v1.md (Q11). PIUse is deliberately absent.
const int kButtonSlots[32] = {
    io::PIFire, io::PIAltFire, io::PIFlareFire, io::PIAction, io::PISprint, io::PICrouch, io::PILie,
    io::PIReload, io::PIToggleCameraMode, io::PICameraMode1, io::PICameraMode2, io::PICameraMode3,
    io::PICameraMode4, io::PIWeaponSelect1, io::PIWeaponSelect2, io::PIWeaponSelect3,
    io::PIWeaponSelect4, io::PIWeaponSelect5, io::PIWeaponSelect6, io::PIWeaponSelect7,
    io::PIWeaponSelect8, io::PIWeaponSelect9, io::PIToggleWeapon, io::PIToggleFireRate,
    io::PIPositionSelect1, io::PIPositionSelect2, io::PIPositionSelect3, io::PIPositionSelect4,
    io::PIPositionSelect5, io::PIPositionSelect6, io::PIPositionSelect7, io::PIPositionSelect8 };

Channel* g_channel = 0;

void logLine(const char* fmt, ...)
{
    char buf[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_channel)
        g_channel->log(buf);
    else
    {
        std::fprintf(stderr, "[serverext] %s\n", buf);
        std::fflush(stderr);
    }
}

IObject* rootOf(IObject* o)
{
    if (!o)
        return 0;
    while (o->getParent())
        o = o->getParent();
    return o;
}

float distance(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The mask PlayerAction::get leaves in a human's PlayerInput: axes 0..5 plus the button table
// (Game/Common/PlayerAction.cpp, playerInputIsMappedArray).
unsigned long long humanMappingMask()
{
    static const int buttons[] = { 32, 34, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                   28, 29, 30,  8, 31, 47,  9, 10, 13, 12, 37, 38, 62, 36, 35, 39,
                                   40, 41, 42, 11, 43, 44, 33, 45, 46 };
    unsigned long long mask = 0;
    for (int axis = 0; axis <= 5; ++axis)
        mask |= 1ULL << axis;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        mask |= 1ULL << buttons[i];
    return mask;
}

std::string sanitizeName(const std::string& in, size_t maxLen)
{
    std::string out = in.substr(0, maxLen);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c < 0x20 || c == 0x7F)
            out[i] = '?';
    }
    return out;
}

std::string templateName(IObject* o)
{
    return o && o->getTemplate() ? o->getTemplate()->getName() : std::string();
}

// The root's PlayerControlObject template interface (vehicle category and type), or null.
IPlayerControlObjectTemplate* pcoTemplateOf(IObject* o)
{
    const IObjectTemplate* t = o ? o->getTemplate() : 0;
    return t ? static_cast<IPlayerControlObjectTemplate*>(
                   const_cast<IObjectTemplate*>(t)->queryInterface(IID_IPlayerControlObjectTemplate))
             : 0;
}

uint32_t netIdOf(IObject* o)
{
    io::INetworkable* net = o ? o->getNetworkable() : 0;
    return net ? net->m_networkableId : 0;
}

// A vehicle in protocol.md's sense: a root object with seats (IPlayerControlObject) that is not a
// soldier. Cameras are excluded too: every player's MultiPlayerFreeCamera is a root
// IPlayerControlObject that its player occupies while dead or spectating.
IPlayerControlObject* vehiclePco(IObject* o)
{
    if (!o || o->getParent() || o->queryInterface(IID_ISoldier) || o->queryInterface(IID_ICameraObject))
        return 0;
    return static_cast<IPlayerControlObject*>(o->queryInterface(IID_IPlayerControlObject));
}

// The vehicle seat a player occupies, or null when on foot, dead or in a non-vehicle object.
IPlayerControlObject* seatOf(world::IPlayer* p, IObject** rootOut)
{
    IObject* vehicle = p->getVehicle();
    IObject* root = rootOf(vehicle);
    if (rootOut)
        *rootOut = root;
    if (!p->getIsAlive() || !vehicle || !vehiclePco(root))
        return 0;
    return static_cast<IPlayerControlObject*>(vehicle->queryInterface(IID_IPlayerControlObject));
}

// Health of a vehicle root as 0..1 from its armour component, or -1 without one. destroyed is set
// when the hit points are at or below 0.
float healthOf(IObject* root, bool* destroyed)
{
    if (destroyed)
        *destroyed = false;
    IArmorGLComp* armor = static_cast<IArmorGLComp*>(root->queryComponent(IID_IArmorGLComp, IID_IArmorGLComp));
    if (!armor)
        return -1.0f;
    const float hp = armor->getHitPoints();
    if (destroyed)
        *destroyed = !(hp > 0.0f);
    const float maxHp = armor->getMaxHitPoints();
    float h = maxHp > 0.0f ? hp / maxHp : 0.0f;
    if (!(h >= 0.0f)) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    return h;
}

float yawOf(const Mat4& m)
{
    return std::atan2(m.m[8], m.m[10]) * 57.29577951f;
}

// Yaw, pitch and roll in protocol.md's conventions from an absolute transformation.
void attitudeOf(const Mat4& m, float ypr[3])
{
    const float r2d = 57.29577951f;
    float s = m.m[9];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    ypr[0] = std::atan2(m.m[8], m.m[10]) * r2d;
    ypr[1] = -std::asin(s) * r2d;
    ypr[2] = std::atan2(m.m[1], m.m[5]) * r2d;
}

// Tick-cost histogram: 10 microsecond buckets up to 20 ms, plus an overflow count.
class CostStats
{
public:
    CostStats() : m_total(0), m_overflow(0), m_maxUs(0), m_windowCount(0)
    {
        for (int i = 0; i < kBuckets; ++i) { m_all[i] = 0; m_window[i] = 0; }
    }

    void add(double us)
    {
        const int b = us < 0 ? 0 : static_cast<int>(us / 10.0);
        if (b >= kBuckets) ++m_overflow; else { ++m_all[b]; ++m_window[b]; }
        ++m_total;
        ++m_windowCount;
        if (us > m_maxUs) m_maxUs = us;
        if (us > m_windowMax) m_windowMax = us;
    }

    void report(unsigned long tick, size_t bots, size_t conns, size_t projectiles, unsigned long launches)
    {
        logLine("tick %lu cost (handler, us): last %u ticks p50 %.0f p99 %.0f max %.0f | run %llu ticks p50 %.0f p99 %.0f max %.0f (>20ms: %llu) | bots %u connections %u projectiles %u (launches %lu)",
             tick, m_windowCount, pct(m_window, m_windowCount, 0.50), pct(m_window, m_windowCount, 0.99), m_windowMax,
             static_cast<unsigned long long>(m_total), pct(m_all, m_total, 0.50), pct(m_all, m_total, 0.99), m_maxUs,
             static_cast<unsigned long long>(m_overflow), static_cast<unsigned>(bots), static_cast<unsigned>(conns),
             static_cast<unsigned>(projectiles), launches);
        for (int i = 0; i < kBuckets; ++i) m_window[i] = 0;
        m_windowCount = 0;
        m_windowMax = 0;
    }

private:
    static const int kBuckets = 2000;

    // Upper edge of the bucket holding the q-quantile.
    static double pct(const unsigned long long* h, unsigned long long n, double q)
    {
        if (!n) return 0;
        const unsigned long long target = static_cast<unsigned long long>(std::ceil(q * n));
        unsigned long long acc = 0;
        for (int i = 0; i < kBuckets; ++i)
        {
            acc += h[i];
            if (acc >= target) return (i + 1) * 10.0;
        }
        return kBuckets * 10.0;
    }

    unsigned long long m_all[kBuckets];
    unsigned long long m_window[kBuckets];
    unsigned long long m_total;
    unsigned long long m_overflow;
    double m_maxUs;
    double m_windowMax = 0;
    unsigned int m_windowCount;
};

struct Bot
{
    int id;
    world::IPlayer* player;           // borrowed; compared with getPlayerFromId(id), never dereferenced first
    uint32_t conn;
    int team;                         // the team the spawn groups below belong to; follows getTeam()
    float axes[proto::AxisCount];      // what is written every tick
    float pending[proto::AxisCount];   // latest SetInput of this tick
    uint32_t buttons;                  // pressed for this tick only
    uint32_t pendingButtons;
    bool hasPending;
    unsigned int idleTicks;
    bool watchdogTripped;
    bool levelAndHold;                 // v2 item 11: the watchdog flies the heli neutral
    LevelAndHold neutral;
    bool wasAlive;
    int removeCountdown;       // > 0 after the sidecar disconnected
    int missingTicks;
    unsigned long lastGroupTry;

    // Spawn fallback (protocol.md v1 "Blocked spawns").
    int home;                     // the pad group; -1 if the team has no on-foot group
    int target;                   // the group the module wants the bot to spawn at
    unsigned long readySince;     // tick the bot became ready to spawn, 0 while not ready
    unsigned long targetSince;    // tick the current target's fallback timer started
    unsigned long lastBlocked;    // tick SpawnBlocked was last sent, 0 if never this wait
    std::vector<int> order;       // home first, then on-foot groups by distance, then enter-on-spawn
    std::vector<bool> tried;
    bool cycled;                  // every group of order was tried or found inactive
    bool groupsPending;           // the team has no usable group; retried every kResolveRetryTicks
    unsigned long lastResolve;    // tick of the last resolution attempt

    int viewMode;                 // last camera view mode seen, -1 if none
    bool firing;
    bool manDown;                 // last man-down state seen (logged)
    unsigned long lastSpot;       // tick of the last accepted Spot, 0 if none
};

// One sidecar connection (protocol.md v2 item 3).
struct Conn
{
    bool ready;                     // Hello answered
    unsigned long vehicleListDue;
    int lastStatus;                 // last RoundState sent; -2 forces a send
    int lastBeginRound;
    bool projectilesShown;          // the last ProjectileList sent had entries
    Conn() : ready(false), vehicleListDue(0), lastStatus(-2), lastBeginRound(-2), projectilesShown(false) {}
};

// A missile or rocket in flight (v2 item 5), validated every tick through the engine's own lookup.
struct Tracked
{
    IObject* object;        // borrowed; used only after getRegisteredObject(objectId) returns it
    unsigned int objectId;
    uint32_t wireId;
    std::string templateName;
    bool guided;
};

class Module : public IEventListener,
               public IServerExtension::IDamageObserver,
               public IServerExtension::IProjectileObserver,
               public IServerExtension::ISpotObserver
{
public:
    Module()
        : m_em(0), m_ext(0), m_players(0), m_objects(0), m_game(0), m_server(0), m_system(0),
          m_tick(0), m_unloadSeen(false), m_mask(humanMappingMask()),
          m_rejectedInputs(0), m_lastRejectLog(0), m_scanKey(0), m_scanFull(true),
          m_lastRestart(0), m_restarted(false) {}

    bool init(ClassManager* cm)
    {
        std::string text;
        std::vector<std::string> diags;
        Config cfg;
        bool cfgOk = true;
        if (readFile("serverext.cfg", text))
            cfgOk = parseConfig(text, cfg, diags);
        else
            logLine("serverext.cfg not found in the working directory; using defaults");
        for (size_t i = 0; i < diags.size(); ++i)
            logLine("serverext.cfg %s", diags[i].c_str());
        if (!cfgOk)
        {
            logLine("serverext.cfg has errors; refusing to load");
            return false;
        }
        m_config = cfg;

        m_em = reinterpret_cast<EventManager*>(cm->getSingleton("EventManager"));
        m_ext = reinterpret_cast<IServerExtension*>(cm->getSingleton("OpenBF2ServerExtension"));
        m_players = reinterpret_cast<PlayerManager*>(cm->getSingleton("PlayerManager"));
        m_objects = reinterpret_cast<ObjectManager*>(cm->getSingleton("ObjectManager"));
        m_game = reinterpret_cast<IGame*>(cm->getSingleton("Game"));
        m_system = reinterpret_cast<System*>(cm->getSingleton("System"));
        m_server = m_game ? static_cast<IGameServer*>(m_game->queryInterface(IID_IGameServer)) : 0;
        if (!m_em || !m_ext || !m_players || !m_objects || !m_game || !m_server)
        {
            logLine("a required engine singleton is missing (EventManager %p, OpenBF2ServerExtension %p, PlayerManager %p, ObjectManager %p, Game %p, IGameServer %p); refusing to load",
                 (void*)m_em, (void*)m_ext, (void*)m_players, (void*)m_objects, (void*)m_game, (void*)m_server);
            return false;
        }

        g_channel = new Channel();   // lives for the process, like the I/O thread
        std::string error;
        if (!startIoThread(m_config, g_channel, error))
        {
            Channel* c = g_channel;
            g_channel = 0;
            logLine("bot channel %s:%d: %s; refusing to load", m_config.listenHost.c_str(), m_config.listenPort, error.c_str());
            (void)c;   // the pipe stays open; the module is unloaded right after
            return false;
        }

        const bool tickOk = m_em->registerEventHandler(kTickCategory, this, 0);
        const bool hudOk = m_em->registerEventHandler(ECHud, this, 0);
        // Registered after GameServer at the same priority, so the module sees the raw sender id
        // before GameServer blanks it for server messages (docs/findings/module-v2.md, Chat).
        const bool chatOk = m_em->registerEventHandler(ECNetwork, this, 0);
        const bool damageOk = m_ext->addDamageObserver(this);
        const bool projectileOk = m_ext->addProjectileObserver(this);
        const bool spotOk = m_ext->addSpotObserver(this);
        const unsigned int handle = m_em->postEventNextUpdate(kTickCategory, kTickEventId, 0);
        logLine("loaded: protocol v%u, bot channel %s:%d (max %d connections), watchdog %d ticks, enterRange %.1f m, "
             "spawnFallback %d s, spawnBlocked %d s, allowRoundRestart %d, allowTeleport %d, projectileRange %.0f m; "
             "tick hook %d (event %u), unload hook %d, chat hook %d, damage %d, projectile %d, spot %d observers, System %p",
             static_cast<unsigned>(proto::kVersion), m_config.listenHost.c_str(), m_config.listenPort, m_config.maxConnections,
             m_config.watchdogTicks, m_config.enterRange, m_config.spawnFallbackSeconds, m_config.spawnBlockedSeconds,
             m_config.allowRoundRestart ? 1 : 0, m_config.allowTeleport ? 1 : 0, m_config.projectileRange,
             tickOk ? 1 : 0, handle, hudOk ? 1 : 0, chatOk ? 1 : 0, damageOk ? 1 : 0, projectileOk ? 1 : 0, spotOk ? 1 : 0,
             (void*)m_system);
        // The listener is already up, so the module cannot back out once the hook is registered.
        return true;
    }

    virtual void handleEvent(EventCategory category, unsigned int id, EventNode* node, void*)
    {
        if (category == ECHud)
        {
            if (id == kUnloadHudEventId)
            {
                m_unloadSeen = true;
                m_vehicles.clear();
                m_tracked.clear();
                m_spotted.clear();
                m_scanFull = true;   // rebuilt with one full walk on the next tick
                logLine("tick %lu: level unload", m_tick);
            }
            return;
        }
        if (category == ECNetwork)
        {
            if (id == kChatEventId && node)
                onChat(static_cast<StringReceivedEvent*>(node));
            return;
        }
        if (category != kTickCategory || id != kTickEventId)
            return;

        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        // Re-post first so the chain survives any early return.
        m_em->postEventNextUpdate(kTickCategory, kTickEventId, 0);
        ++m_tick;
        processCommands();
        scanVehicles();
        updateBots();
        sendState();
        sendProjectiles();
        sendVehicleList();
        sendRoundState();
        m_unloadSeen = m_unloadSeen && hasMissingBots();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        m_cost.add(us);
        if (m_tick % kStatsPeriodTicks == 0)
            m_cost.report(m_tick, m_bots.size(), m_conns.size(), m_tracked.size(), m_launches);
    }

    // ---------------------------------------------------------------- engine observers (v2)

    // R5 (IServerExtension::IDamageObserver). Kills go to every connection (the kill feed); damage
    // only to connections whose bots are the victim or the attacker (docs/findings/module-v2.md).
    virtual void onDamage(const IServerExtension::DamageInfo& d)
    {
        if (!anyReady())
            return;
        for (int k = 0; k < d.killedCount; ++k)
            if (d.killed[k])
                sendKill(d.killed[k], d.attacker, d.vehicle && !d.weapon ? d.vehicle : d.weapon);

        IObject* victimRoot = rootOf(d.victim);
        proto::DamageEvent e;
        bool built = false;
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            if (!c->second.ready)
                continue;
            uint8_t flags = 0;
            // serverGiveDamage kills the occupants before the armour takes the damage, so a bot
            // killed by this damage has already left the vehicle: count the killed list too.
            bool victimOwn = victimRoot && hasBotIn(c->first, victimRoot);
            for (int k = 0; k < d.killedCount && !victimOwn; ++k)
                victimOwn = d.killed[k] && isOwnBot(d.killed[k], c->first);
            if (victimOwn)
                flags |= proto::kDamageVictimOwn;
            if (d.attacker && isOwnBot(d.attacker, c->first))
                flags |= proto::kDamageAttackerOwn;
            if (!flags)
                continue;
            if (!built)
            {
                fillDamage(d, victimRoot, e);
                built = true;
            }
            e.flags = flags | (e.flags & ~(proto::kDamageVictimOwn | proto::kDamageAttackerOwn));
            std::vector<uint8_t> f;
            proto::encodeDamage(f, e);
            g_channel->sendControl(c->first, f);
        }
    }

    // v2 item 5 (IServerExtension::IProjectileObserver): keep missiles and rockets, the projectiles
    // with a networkable id or a seek component. Bullets have neither on the dedicated server.
    virtual void onProjectileLaunched(IObject* projectile, world::IPlayer*)
    {
        ++m_launches;
        if (!projectile || !anyReady() || m_tracked.size() >= kMaxTrackedProjectiles)
            return;
        const uint32_t net = netIdOf(projectile);
        const bool guided = projectile->queryComponent(IID_IProjectileSeekComp, IID_IProjectileSeekComp) != 0;
        // Unguided rockets (the Hydra and S-8 pods) have neither a networkable id nor a seek
        // component; what sets them apart from bullets is the detonation component.
        const bool detonates =
            projectile->queryComponent(IID_IProjectileDetonationComp, IID_IProjectileDetonationComp) != 0;
        if (!net && !guided && !detonates)
        {
            if (++m_skippedLogs <= 20)
                logLine("tick %lu: projectile '%s' launched without a networkable id, seek or detonation component; not tracked",
                     m_tick, templateName(projectile).c_str());
            return;
        }
        Tracked t;
        t.object = projectile;
        t.objectId = projectile->getObjectId();
        t.wireId = net ? net : kNoNetIdBase + t.objectId;
        t.templateName = templateName(projectile);
        t.guided = guided;
        m_tracked.push_back(t);
        if (++m_projectileLogs <= 20 || m_projectileLogs % 200 == 0)
            logLine("tick %lu: projectile %u '%s' launched (net id %u, guided %d, %lu so far)", m_tick, t.wireId,
                 t.templateName.c_str(), net, guided ? 1 : 0, m_projectileLogs);
    }

    // v2 item 6 (IServerExtension::ISpotObserver): a target stays spotted for its spotting team for
    // the client's marker lifetime.
    virtual void onSpotted(const IServerExtension::SpotInfo& s)
    {
        if (s.team < 0 || s.team > 3 || !s.targetNetId)
            return;
        Spotted& sp = m_spotted[s.targetNetId];
        sp.until[s.team] = m_tick + kSpottedTicks;
        if (++m_spotLogs <= 50 || m_spotLogs % 100 == 0)
            logLine("tick %lu: object %u spotted by team %d (connection %d, spotter %u)", m_tick, s.targetNetId, s.team,
                 s.connectionId, s.spotterNetId);
    }

private:
    struct Spotted
    {
        unsigned long until[4];
        Spotted() { until[0] = until[1] = until[2] = until[3] = 0; }
    };

    // ---------------------------------------------------------------- outbound helpers

    bool anyReady() const
    {
        for (std::map<uint32_t, Conn>::const_iterator c = m_conns.begin(); c != m_conns.end(); ++c)
            if (c->second.ready)
                return true;
        return false;
    }

    bool ready(uint32_t conn) const
    {
        std::map<uint32_t, Conn>::const_iterator c = m_conns.find(conn);
        return c != m_conns.end() && c->second.ready;
    }

    void control(uint32_t conn, std::vector<uint8_t>& frame) { g_channel->sendControl(conn, frame); }

    void error(uint32_t conn, uint32_t requestId, uint16_t code, const std::string& message)
    {
        std::vector<uint8_t> f;
        proto::encodeError(f, requestId, code, message);
        control(conn, f);
    }

    void ack(uint32_t conn, uint32_t requestId)
    {
        std::vector<uint8_t> f;
        proto::encodeAck(f, requestId);
        control(conn, f);
    }

    void botRemoved(const Bot& b, uint8_t reason)
    {
        if (!ready(b.conn))
            return;
        std::vector<uint8_t> f;
        proto::encodeBotRemoved(f, static_cast<uint8_t>(b.id), reason);
        control(b.conn, f);
    }

    // ---------------------------------------------------------------- bot lookup

    world::IPlayer* live(const Bot& b) const
    {
        world::IPlayer* p = m_players->getPlayerFromId(b.id);
        return p == b.player ? p : 0;
    }

    Bot* ownBot(int id, uint32_t conn)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].id == id && m_bots[i].conn == conn && m_bots[i].removeCountdown < 0)
                return &m_bots[i];
        return 0;
    }

    bool isOwnBot(world::IPlayer* p, uint32_t conn) const
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].player == p && m_bots[i].conn == conn && m_bots[i].removeCountdown < 0)
                return true;
        return false;
    }

    // True when a live bot of conn is the object root or sits in it.
    bool hasBotIn(uint32_t conn, IObject* root)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            const Bot& b = m_bots[i];
            if (b.conn != conn || b.removeCountdown >= 0)
                continue;
            world::IPlayer* p = live(b);
            if (p && rootOf(p->getVehicle()) == root)
                return true;
        }
        return false;
    }

    bool hasBotOnTeam(uint32_t conn, int team)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            const Bot& b = m_bots[i];
            world::IPlayer* p = b.conn == conn && b.removeCountdown < 0 ? live(b) : 0;
            if (p && p->getTeam() == team)
                return true;
        }
        return false;
    }

    bool hasMissingBots() const
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].missingTicks > 0)
                return true;
        return false;
    }

    // The player whose soldier is root, or the occupant of a vehicle root's first seat.
    world::IPlayer* playerOfRoot(IObject* root)
    {
        if (!root)
            return 0;
        if (root->queryInterface(IID_ISoldier))
        {
            const std::list<world::IPlayer*>& players = m_players->getPlayers();
            for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
                if (*it && (*it)->getDefaultVehicle() == root)
                    return *it;
            return 0;
        }
        IPlayerControlObject* pco = vehiclePco(root);
        if (!pco)
            return 0;
        if (world::IPlayer* p = pco->getOccupingPlayer())
            return p;
        std::map<int, IPlayerControlObject*>* seats = pco->getPcos();
        if (seats)
            for (std::map<int, IPlayerControlObject*>::iterator s = seats->begin(); s != seats->end(); ++s)
                if (s->second && s->second->getOccupingPlayer())
                    return s->second->getOccupingPlayer();
        return 0;
    }

    // The team that owns root: a vehicle's team, or a soldier's player's team; 0 if neither.
    int teamOfRoot(IObject* root)
    {
        IPlayerControlObject* pco = vehiclePco(root);
        if (pco && pco->getTeam() > 0)
            return pco->getTeam();
        world::IPlayer* p = playerOfRoot(root);
        return p ? p->getTeam() : 0;
    }

    // ---------------------------------------------------------------- inbound

    void processCommands()
    {
        g_channel->drainCommands(m_inbox);
        for (std::deque<Command>::iterator it = m_inbox.begin(); it != m_inbox.end(); ++it)
        {
            Command& c = *it;
            if (c.kind == Command::Connected)
            {
                m_conns[c.conn] = Conn();
                continue;
            }
            if (c.kind == Command::Disconnected)
            {
                if (m_conns.count(c.conn))
                    onDisconnect(c.conn);
                continue;
            }
            if (!m_conns.count(c.conn))
                continue;   // left over from a closed connection
            switch (c.kind)
            {
            case Command::Hello:        onHello(c.conn, c.hello); break;
            case Command::CreateBot:    onCreateBot(c.conn, c.createBot); break;
            case Command::EnterVehicle: onEnterVehicle(c.conn, c.enterVehicle); break;
            case Command::RemoveBot:    onRemoveBot(c.conn, c.removeBot); break;
            case Command::SetInput:     onSetInput(c.conn, c.setInput); break;
            case Command::RestartRound: onRestartRound(c.conn, c.restartRound); break;
            case Command::Teleport:     onTeleport(c.conn, c.teleport); break;
            case Command::Spot:         onSpot(c.conn, c.spot); break;
            default: break;
            }
        }
        m_inbox.clear();
    }

    void onDisconnect(uint32_t conn)
    {
        size_t n = 0;
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            Bot& b = m_bots[i];
            if (b.conn != conn || b.removeCountdown >= 0)
                continue;
            b.hasPending = false;
            neutralise(b, live(b));
            b.removeCountdown = m_config.watchdogTicks;
            ++n;
        }
        logLine("tick %lu: sidecar connection %u gone; %u bots neutralised, removal in %d ticks",
             m_tick, conn, static_cast<unsigned>(n), m_config.watchdogTicks);
        m_conns.erase(conn);
    }

    void onHello(uint32_t conn, const proto::HelloMsg& m)
    {
        const std::string map = m_game->getCurrentMapName();
        const std::string mode = m_game->getCurrentMapGameMode();
        const uint8_t features = (m_config.allowTeleport ? proto::kFeatureTeleport : 0) |
                                 (m_config.allowRoundRestart ? proto::kFeatureRoundRestart : 0);
        std::vector<uint8_t> f;
        proto::encodeWelcome(f, kTickRate, static_cast<uint32_t>(m_tick), map, mode, m_config.enterRange,
                             m_config.projectileRange, features);
        control(conn, f);
        Conn& c = m_conns[conn];
        c.ready = true;
        c.vehicleListDue = m_tick + 1;   // the first tick after Welcome
        c.lastStatus = c.lastBeginRound = -2;   // RoundState on the next tick
        logLine("tick %lu: Hello from '%s' on connection %u; Welcome map '%s' mode '%s'", m_tick,
             sanitizeName(m.name, 64).c_str(), conn, map.c_str(), mode.c_str());
    }

    void onCreateBot(uint32_t conn, const proto::CreateBotMsg& m)
    {
        if (m.team != 1 && m.team != 2)
        {
            error(conn, m.requestId, proto::ErrInvalidValue, "team must be 1 or 2");
            return;
        }
        const std::string name = sanitizeName(m.name, 64);
        if (name.empty())
        {
            error(conn, m.requestId, proto::ErrInvalidValue, "name is empty");
            return;
        }
        // protocol.md v1: CreateBot succeeds whenever the team has a spawn group a bot can use,
        // active or not; a blocked one is handled by the spawn fallback.
        const int groupCount = listGroups(m.team);
        const spawn::Choice choice = spawn::choose(m_groups, groupCount);
        if (!choice.ok)
        {
            error(conn, m.requestId, proto::ErrRefused, "the team has no spawn group (is a level loaded?)");
            logLine("tick %lu: CreateBot '%s' team %d refused: %d spawn groups, none usable", m_tick, name.c_str(),
                 m.team, groupCount);
            return;
        }
        const int id = m_ext->createInputPlayer(name);
        world::IPlayer* p = id >= 0 ? m_players->getPlayerFromId(id) : 0;
        if (!p || id > 255)
        {
            error(conn, m.requestId, proto::ErrRefused, "the engine refused to create the player (player limit?)");
            logLine("tick %lu: CreateBot '%s' refused: createInputPlayer -> %d", m_tick, name.c_str(), id);
            return;
        }
        // Python's PlayerConnect handlers ran inside createInputPlayer, and mm_autobalance may have
        // put the bot on a team already (docs/findings/q5-q9.md, 4). The requested team wins.
        const int connectTeam = p->getTeam();
        p->setTeam(m.team, false);
        p->setKit(kDefaultKit);
        if (p->getTeam() != m.team)
            logLine("tick %lu: bot %d: setTeam(%d) left it on team %d", m_tick, id, m.team, p->getTeam());
        else if (connectTeam != m.team)
            logLine("tick %lu: bot %d: moved from team %d (set during PlayerConnect) to the requested team %d", m_tick, id,
                 connectTeam, m.team);

        Bot b;
        b.id = id;
        b.player = p;
        b.conn = conn;
        b.team = m.team;
        for (int i = 0; i < proto::AxisCount; ++i)
            b.axes[i] = b.pending[i] = 0.0f;
        b.buttons = b.pendingButtons = 0;
        b.hasPending = false;
        b.idleTicks = 0;
        b.watchdogTripped = false;
        b.levelAndHold = false;
        b.wasAlive = p->getIsAlive();
        b.removeCountdown = -1;
        b.missingTicks = 0;
        b.lastGroupTry = m_tick;
        b.home = choice.home;
        b.viewMode = -1;
        b.firing = false;
        b.manDown = false;
        b.lastSpot = 0;
        resetFallback(b);
        b.target = choice.target;
        b.groupsPending = false;
        b.lastResolve = m_tick;
        p->setSpawnGroup(b.target);
        m_bots.push_back(b);

        std::vector<uint8_t> f;
        proto::encodeBotCreated(f, m.requestId, static_cast<uint8_t>(id));
        control(conn, f);
        logLine("tick %lu: created bot %d '%s' team %d kit %d home spawn group %d target %d for connection %u (ai %d connected %d)",
             m_tick, id, p->getName().c_str(), m.team, kDefaultKit, choice.home, m_bots.back().target, conn,
             p->getIsAIPlayer() ? 1 : 0, p->getIsConnected() ? 1 : 0);
        logGroups(m.team, m_groups, groupCount);
    }

    // The seat object with PCO id seat on vehicle root, if that seat is free and the vehicle's
    // team is 0 or team.
    IObject* freeSeat(IObject* root, int seat, int team, const char** why)
    {
        IPlayerControlObject* pco = vehiclePco(root);
        if (!pco)
        {
            *why = "not a vehicle";
            return 0;
        }
        const int vehicleTeam = pco->getTeam();
        if (vehicleTeam != 0 && vehicleTeam != team)
        {
            *why = "vehicle belongs to the other team";
            return 0;
        }
        std::map<int, IPlayerControlObject*>* seats = pco->getPcos();
        if (!seats)
        {
            *why = "vehicle has no seats";
            return 0;
        }
        std::map<int, IPlayerControlObject*>::iterator s = seats->find(seat);
        if (s == seats->end() || !s->second)
        {
            *why = "no such seat";
            return 0;
        }
        if (s->second->isOccupied())
        {
            *why = "seat is occupied";
            return 0;
        }
        return static_cast<IObject*>(s->second->queryInterface(IID_IObject));
    }

    void onEnterVehicle(uint32_t conn, const proto::EnterVehicleMsg& m)
    {
        Bot* b = ownBot(m.playerId, conn);
        world::IPlayer* p = b ? live(*b) : 0;
        if (!p)
        {
            error(conn, m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
            return;
        }
        IObject* me = rootOf(p->getVehicle());
        if (!p->getIsAlive() || !me || !me->queryInterface(IID_ISoldier))
        {
            error(conn, m.requestId, proto::ErrRefused, "the bot must be alive and on foot");
            return;
        }

        // The live team, not the cached one: an admin may have switched it since the last tick.
        const int team = p->getTeam();

        // protocol.md v1: both forms skip destroyed vehicles and require the bot within enterRange.
        const Vec3& myPos = me->getAbsolutePosition();
        const char* why = "no undestroyed vehicle with that seat free within enterRange";
        IObject* seat = 0;
        if (m.vehicleId != 0)
        {
            if (m.vehicleId > 0xFFFF)
            {
                error(conn, m.requestId, proto::ErrUnavailable, "vehicle id out of range");
                return;
            }
            const unsigned short netId = static_cast<unsigned short>(m.vehicleId);
            IObject* root = rootOf(m_game->getObjectFromNetworkableId(netId));
            if (!root)
            {
                error(conn, m.requestId, proto::ErrUnavailable, "no object with that networkable id");
                return;
            }
            bool destroyed = false;
            const bool isVehicle = vehiclePco(root) != 0;
            if (isVehicle)
                healthOf(root, &destroyed);
            if (!isVehicle)
                why = "not a vehicle";
            else if (destroyed)
                why = "the vehicle is destroyed";
            else if (distance(root->getAbsolutePosition(), myPos) > m_config.enterRange)
                why = "the vehicle is out of enterRange";
            else
                seat = freeSeat(root, m.seat, team, &why);
        }
        else
        {
            float best = m_config.enterRange;
            for (std::map<unsigned short, IObject*>::iterator it = m_vehicles.begin(); it != m_vehicles.end(); ++it)
            {
                IObject* o = liveVehicle(it->first, it->second);
                if (!o)
                    continue;
                const float d = distance(o->getAbsolutePosition(), myPos);
                if (d > best)
                    continue;
                bool destroyed = false;
                healthOf(o, &destroyed);
                if (destroyed)
                    continue;
                const char* ignored = 0;
                IObject* s = freeSeat(o, m.seat, team, &ignored);
                if (s)
                {
                    best = d;
                    seat = s;
                }
            }
        }
        if (!seat)
        {
            error(conn, m.requestId, proto::ErrUnavailable, why);
            logLine("tick %lu: EnterVehicle bot %d vehicle %u seat %u refused: %s", m_tick, b->id, m.vehicleId, m.seat, why);
            return;
        }

        const bool ok = m_game->enterVehicle(p, seat, false);
        if (!ok || p->getVehicle() != seat)
        {
            error(conn, m.requestId, proto::ErrRefused, "IGame::enterVehicle did not seat the bot");
            logLine("tick %lu: EnterVehicle bot %d: enterVehicle -> %d, vehicle %p, wanted %p", m_tick, b->id, ok ? 1 : 0,
                 (void*)p->getVehicle(), (void*)seat);
            return;
        }
        IObject* root = rootOf(seat);
        logLine("tick %lu: bot %d entered '%s' (net id %u) seat %u", m_tick, b->id, templateName(root).c_str(),
             netIdOf(root), m.seat);
        ack(conn, m.requestId);
    }

    void onRemoveBot(uint32_t conn, const proto::RemoveBotMsg& m)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            Bot& b = m_bots[i];
            if (b.id != m.playerId || b.conn != conn || b.removeCountdown >= 0)
                continue;
            world::IPlayer* p = live(b);
            const Bot copy = b;
            m_bots.erase(m_bots.begin() + i);
            if (p)
                m_server->destroyPlayer(p);
            logLine("tick %lu: bot %d removed on request", m_tick, copy.id);
            ack(conn, m.requestId);
            botRemoved(copy, proto::ReasonRemoveBot);
            return;
        }
        error(conn, m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
    }

    void onSetInput(uint32_t conn, const proto::SetInputMsg& m)
    {
        Bot* b = ownBot(m.playerId, conn);
        if (!b)
        {
            error(conn, 0, proto::ErrUnknownPlayer, "SetInput for a player that is not a bot of this connection");
            return;
        }
        float axes[proto::AxisCount];
        for (int i = 0; i < proto::AxisCount; ++i)
        {
            if (!proto::conditionAxis(m.axes[i], axes[i]))
            {
                error(conn, 0, proto::ErrInvalidValue, "SetInput axis is NaN or infinite; input not applied");
                ++m_rejectedInputs;
                if (m_tick - m_lastRejectLog >= 30)
                {
                    logLine("tick %lu: rejected non-finite SetInput for bot %d (%lu rejected so far)", m_tick, b->id, m_rejectedInputs);
                    m_lastRejectLog = m_tick;
                }
                return;
            }
        }
        // Every one of the 32 bits is in the button table (kButtonSlots), so there is no unknown
        // bit to ignore and log.
        for (int i = 0; i < proto::AxisCount; ++i)
            b->pending[i] = axes[i];   // latest wins within a tick, buttons included
        b->pendingButtons = m.buttons;
        b->hasPending = true;
    }

    void onRestartRound(uint32_t conn, const proto::RestartRoundMsg& m)
    {
        if (!m_config.allowRoundRestart)
        {
            error(conn, m.requestId, proto::ErrRefused, "round restart is disabled (serverext.cfg allowRoundRestart 0)");
            logLine("tick %lu: RestartRound refused: allowRoundRestart is 0", m_tick);
            return;
        }
        if (m_restarted && m_tick - m_lastRestart < kRestartMinTicks)
        {
            error(conn, m.requestId, proto::ErrRefused, "at most one round restart per 60 s");
            logLine("tick %lu: RestartRound refused: last restart %lu ticks ago", m_tick, m_tick - m_lastRestart);
            return;
        }
        if (!m_ext->restartRound())
        {
            error(conn, m.requestId, proto::ErrRefused, "the engine refused the round restart (no game logic)");
            logLine("tick %lu: RestartRound refused by the engine", m_tick);
            return;
        }
        m_restarted = true;
        m_lastRestart = m_tick;
        logLine("tick %lu: round restarted on request of connection %u (admin.restartMap)", m_tick, conn);
        ack(conn, m.requestId);
    }

    // v2 item 9. Moves the bot's root object to a level attitude at the given yaw and stops it.
    void onTeleport(uint32_t conn, const proto::TeleportMsg& m)
    {
        if (!m_config.allowTeleport)
        {
            error(conn, m.requestId, proto::ErrRefused, "teleport is disabled (serverext.cfg allowTeleport 0)");
            return;
        }
        Bot* b = ownBot(m.playerId, conn);
        world::IPlayer* p = b ? live(*b) : 0;
        if (!p)
        {
            error(conn, m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
            return;
        }
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(m.pos[k]))
            {
                error(conn, m.requestId, proto::ErrInvalidValue, "position is NaN or infinite");
                return;
            }
        if (!std::isfinite(m.yaw) || std::fabs(m.pos[0]) > 8192.0f || std::fabs(m.pos[2]) > 8192.0f ||
            m.pos[1] < -1000.0f || m.pos[1] > 10000.0f)
        {
            error(conn, m.requestId, proto::ErrInvalidValue, "position or yaw out of range");
            return;
        }
        IObject* root = rootOf(p->getVehicle());
        if (!p->getIsAlive() || !root || root->queryInterface(IID_ICameraObject))
        {
            error(conn, m.requestId, proto::ErrRefused, "the bot must be alive");
            return;
        }
        const float y = m.yaw * 0.01745329252f;
        const float s = std::sin(y), c = std::cos(y);
        Mat4 t;
        for (int k = 0; k < 16; ++k)
            t.m[k] = 0.0f;
        t.m[0] = c;  t.m[2] = -s;    // right
        t.m[5] = 1.0f;               // up
        t.m[8] = s;  t.m[10] = c;    // forward: yaw 0 = north (+z), 90 = east (+x)
        t.m[12] = m.pos[0]; t.m[13] = m.pos[1]; t.m[14] = m.pos[2];
        t.m[15] = 1.0f;
        root->setAbsoluteTransformation(t);
        IPhysicsNode* node = static_cast<IPhysicsNode*>(root->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
        if (node)
        {
            node->reset(true);   // zero speeds, re-anchor the last position (ObjectSpawner's order)
            node->setIsAwake();
        }
        logLine("tick %lu: bot %d teleported '%s' (net id %u) to %.1f %.1f %.1f yaw %.1f%s", m_tick, b->id,
             templateName(root).c_str(), netIdOf(root), m.pos[0], m.pos[1], m.pos[2], m.yaw,
             node ? "" : " (no physics node)");
        ack(conn, m.requestId);
    }

    // v2 item 6. What a human's Q + click sends: the spotted message and the "spotted" radio line.
    void onSpot(uint32_t conn, const proto::SpotMsg& m)
    {
        Bot* b = ownBot(m.playerId, conn);
        world::IPlayer* p = b ? live(*b) : 0;
        if (!p)
        {
            error(conn, m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
            return;
        }
        if (!p->getIsAlive() || p->getIsManDown())
        {
            error(conn, m.requestId, proto::ErrRefused, "the bot must be alive and not man-down");
            return;
        }
        if (b->lastSpot && m_tick - b->lastSpot < kSpotMinTicks)
        {
            error(conn, m.requestId, proto::ErrRefused, "at most one Spot per bot per second");
            return;
        }
        IObject* target = m.targetId && m.targetId <= 0xFFFF
                              ? rootOf(m_game->getObjectFromNetworkableId(static_cast<unsigned short>(m.targetId))) : 0;
        const uint32_t rootNet = netIdOf(target);
        if (!target || !rootNet)
        {
            error(conn, m.requestId, proto::ErrUnavailable, "no object with that networkable id");
            return;
        }
        const int targetTeam = teamOfRoot(target);
        if (targetTeam <= 0 || targetTeam == p->getTeam())
        {
            error(conn, m.requestId, proto::ErrUnavailable, "the target is not an enemy");
            return;
        }
        const Vec3& pos = target->getAbsolutePosition();
        Vec2 at;
        at.x = pos.x;
        at.y = pos.z;
        IPlayerControlObjectTemplate* pt = pcoTemplateOf(target);
        const int type = pt ? pt->getVehicleType() : 0;
        const unsigned short net = static_cast<unsigned short>(rootNet);
        m_game->sendSpottedMessage(p, net, at, static_cast<world::VehicleType>(type > 0 ? type : 0));
        m_game->radioMessage(static_cast<TargetChannel>(1), m_game->getSpottedMessage(net), p, 0, true, false);
        b->lastSpot = m_tick;
        logLine("tick %lu: bot %d spotted '%s' (net id %u, team %d)", m_tick, b->id, templateName(target).c_str(), rootNet,
             targetTeam);
        ack(conn, m.requestId);
    }

    // v2 item 7. Chat as GameServer relays it: channels 0 (Global) and 12 (ServerMessage) to every
    // connection; 1 (Team) to connections with a bot on the sender's team; 13 (ServerTeamMessage) to
    // connections with a bot on the team in the event's flags (GameServer::sayTeam); others not at all.
    void onChat(StringReceivedEvent* e)
    {
        if (!anyReady())
            return;
        const int channel = e->m_channel;
        if (channel != 0 && channel != 1 && channel != 12 && channel != 13)
            return;
        world::IPlayer* sender = e->m_senderId >= 0 ? m_players->getPlayerFromId(e->m_senderId) : 0;
        const int team = channel == 13 ? e->m_flags : sender ? sender->getTeam() : 0;
        const bool teamOnly = channel == 1 || channel == 13;
        const std::string text = e->m_message ? std::string(reinterpret_cast<const char*>(e->m_message),
                                                            e->m_messageLength > 1024 ? 1024 : e->m_messageLength)
                                              : std::string();
        const uint8_t senderId = sender ? static_cast<uint8_t>(sender->getId()) : 0;
        const uint8_t chatFlags = sender ? proto::kChatFromPlayer : 0;
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            if (!c->second.ready || (teamOnly && !hasBotOnTeam(c->first, team)))
                continue;
            std::vector<uint8_t> f;
            proto::encodeChat(f, static_cast<uint32_t>(m_tick), chatFlags, senderId, static_cast<uint8_t>(team > 0 ? team : 0),
                              static_cast<uint8_t>(channel), text);
            control(c->first, f);
        }
        if (++m_chatLogs <= 50 || m_chatLogs % 100 == 0)
            logLine("tick %lu: chat channel %d from %d: %s", m_tick, channel, e->m_senderId, sanitizeName(text, 120).c_str());
    }

    // ---------------------------------------------------------------- damage and kills (v2 item 8)

    void fillDamage(const IServerExtension::DamageInfo& d, IObject* victimRoot, proto::DamageEvent& e)
    {
        e.tick = static_cast<uint32_t>(m_tick);
        e.flags = 0;
        if (!(d.hitPointsAfter > 0.0f))
            e.flags |= proto::kDamageVictimDestroyed;
        if (d.killedCount > 0)
            e.flags |= proto::kDamageKilled;
        world::IPlayer* vp = playerOfRoot(victimRoot);
        if (!vp && d.killedCount > 0)
            vp = d.killed[0];   // the occupant this damage killed has already left the vehicle
        e.victim = vp ? static_cast<uint8_t>(vp->getId()) : 0;
        if (vp)
            e.flags |= proto::kDamageHasVictimPlayer;
        e.victimRootId = netIdOf(victimRoot);
        e.victimTemplate = templateName(d.victim);
        e.attacker = d.attacker ? static_cast<uint8_t>(d.attacker->getId()) : 0;
        if (d.attacker)
            e.flags |= proto::kDamageHasAttacker;
        e.weaponTemplate = templateName(d.weapon);
        e.sourceTemplate = templateName(d.source);
        e.amount = d.amount;
        e.before = d.hitPointsBefore;
        e.after = d.hitPointsAfter;
        e.max = d.maxHitPoints;
        for (int k = 0; k < 3; ++k)
            e.pos[k] = d.position[k];
    }

    void sendKill(world::IPlayer* victim, world::IPlayer* attacker, IObject* weapon)
    {
        proto::KillEvent k;
        k.tick = static_cast<uint32_t>(m_tick);
        k.victim = static_cast<uint8_t>(victim->getId());
        k.victimTeam = static_cast<uint8_t>(victim->getTeam());
        k.attacker = attacker ? static_cast<uint8_t>(attacker->getId()) : 0;
        k.attackerTeam = attacker ? static_cast<uint8_t>(attacker->getTeam()) : 0;
        k.weaponTemplate = templateName(weapon);
        const bool teamKill = attacker && attacker != victim && attacker->getTeam() == victim->getTeam();
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            if (!c->second.ready)
                continue;
            k.flags = (isOwnBot(victim, c->first) ? proto::kKillVictimOwn : 0) |
                      (attacker && isOwnBot(attacker, c->first) ? proto::kKillAttackerOwn : 0) |
                      (teamKill ? proto::kKillTeamKill : 0) | (attacker ? proto::kKillHasAttacker : 0);
            std::vector<uint8_t> f;
            proto::encodeKill(f, k);
            control(c->first, f);
        }
        logLine("tick %lu: kill: player %d (team %d) by %d with '%s'%s", m_tick, victim->getId(), victim->getTeam(),
             attacker ? attacker->getId() : -1, k.weaponTemplate.c_str(), teamKill ? " (team kill)" : "");
    }

    // ---------------------------------------------------------------- per-tick bot work

    // Watchdog neutral. In a heli's pilot seat (v2 item 11) the level-and-hold law takes over from
    // the last collective; in other vehicle seats cyclic, pedals and look are 0 and collective is
    // held (v1). On foot every axis is 0. No buttons.
    void neutralise(Bot& b, world::IPlayer* p)
    {
        IObject* root = 0;
        IPlayerControlObject* seat = p ? seatOf(p, &root) : 0;
        const float collective = b.axes[proto::AxisCollective];
        for (int k = 0; k < proto::AxisCount; ++k)
            b.axes[k] = 0.0f;
        b.levelAndHold = false;
        if (seat)
        {
            b.axes[proto::AxisCollective] = collective;
            IPlayerControlObjectTemplate* pt = pcoTemplateOf(root);
            if (seat->getPcoId() == 0 && pt && pt->getVehicleCategory() == kCategoryHelicopter)
            {
                b.levelAndHold = true;
                b.neutral.start(collective);
            }
        }
        b.buttons = 0;
        b.watchdogTripped = true;
    }

    // One tick of the level-and-hold law from the heli's current attitude and vertical speed.
    void flyNeutral(Bot& b, world::IPlayer* p)
    {
        IObject* root = 0;
        if (!b.levelAndHold || !seatOf(p, &root) || !root)
        {
            if (b.levelAndHold)
                for (int k = 0; k < proto::AxisCount; ++k)
                    b.axes[k] = 0.0f;   // left the heli or died: back to all axes 0
            b.levelAndHold = false;
            return;
        }
        float ypr[3];
        attitudeOf(root->getAbsoluteTransformation(), ypr);
        IPhysicsNode* node = static_cast<IPhysicsNode*>(root->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
        const float vy = node ? node->getPositionalSpeed().y : 0.0f;
        const LevelAndHold::Output o = b.neutral.step(ypr[0], ypr[1], ypr[2], vy, 1.0f / kTickRate);
        float v = 0.0f;
        b.axes[proto::AxisPitch] = proto::conditionAxis(o.pitch, v) ? v : 0.0f;
        b.axes[proto::AxisRoll] = proto::conditionAxis(o.roll, v) ? v : 0.0f;
        b.axes[proto::AxisCollective] = proto::conditionAxis(o.collective, v) ? v : 0.0f;
        b.axes[proto::AxisPedals] = proto::conditionAxis(o.pedals, v) ? v : 0.0f;
        b.axes[proto::AxisLookYaw] = b.axes[proto::AxisLookPitch] = 0.0f;
    }

    void writeInput(const Bot& b, world::IPlayer* p)
    {
        io::PlayerInput* in = static_cast<io::PlayerInput*>(p->getAIPlayerInput());
        if (!in)
            return;
        for (int i = 0; i < 64; ++i)
            in->inputs[i] = 0.0f;
        in->mappingFlags = m_mask;
        in->inputs[io::PIPitch] = b.axes[proto::AxisPitch];
        in->inputs[io::PIRoll] = b.axes[proto::AxisRoll];
        in->inputs[io::PIThrottle] = b.axes[proto::AxisCollective];
        in->inputs[io::PIYaw] = b.axes[proto::AxisPedals];          // strafe on foot (Q10)
        in->inputs[io::PIMouseLookX] = b.axes[proto::AxisLookYaw];  // turn on foot
        in->inputs[io::PIMouseLookY] = b.axes[proto::AxisLookPitch];
        for (int bit = 0; bit < 32; ++bit)
            if (b.buttons & (1u << bit))
                in->inputs[kButtonSlots[bit]] = 1.0f;
    }

    void updateBots()
    {
        for (size_t i = 0; i < m_bots.size();)
        {
            Bot& b = m_bots[i];
            world::IPlayer* p = live(b);
            if (!p)
            {
                // Destroyed by the engine. A level unload announces itself first (possibly as a
                // queued event that lands after this handler), so wait one tick before calling it
                // an engine or admin removal.
                ++b.missingTicks;
                if (b.removeCountdown >= 0 || m_unloadSeen || b.missingTicks >= 2)
                {
                    const uint8_t reason = m_unloadSeen ? proto::ReasonMapChange : proto::ReasonEngine;
                    logLine("tick %lu: bot %d no longer exists (%s); not destroying it again", m_tick, b.id,
                         reason == proto::ReasonMapChange ? "level unload" : "removed by the engine or an admin");
                    if (b.removeCountdown < 0)
                        botRemoved(b, reason);
                    m_bots.erase(m_bots.begin() + i);
                    continue;
                }
                ++i;
                continue;
            }
            b.missingTicks = 0;

            if (b.removeCountdown >= 0)
            {
                if (b.removeCountdown == 0)
                {
                    logLine("tick %lu: bot %d removed after its sidecar disconnected", m_tick, b.id);
                    m_bots.erase(m_bots.begin() + i);
                    m_server->destroyPlayer(p);
                    continue;
                }
                --b.removeCountdown;
                flyNeutral(b, p);
                writeInput(b, p);
                ++i;
                continue;
            }

            const int team = p->getTeam();
            if (team != b.team)
                onTeamChanged(b, p, team);
            else if (b.groupsPending && m_tick - b.lastResolve >= kResolveRetryTicks && resolveGroups(b, p))
                logLine("tick %lu: bot %d: team %d has a usable spawn group again; home %d target %d", m_tick, b.id,
                     b.team, b.home, b.target);

            const bool alive = p->getIsAlive();
            if (alive != b.wasAlive)
            {
                if (alive)
                    onSpawned(b, p);
                else
                {
                    logLine("tick %lu: bot %d died", m_tick, b.id);
                    for (int k = 0; k < proto::AxisCount; ++k)
                        b.axes[k] = 0.0f;
                    b.levelAndHold = false;
                    b.viewMode = -1;
                    resetFallback(b);
                }
                b.wasAlive = alive;
            }
            if (!alive)
                updateSpawnFallback(b, p);
            else
                observe(b, p);

            if (b.hasPending)
            {
                for (int k = 0; k < proto::AxisCount; ++k)
                    b.axes[k] = b.pending[k];
                b.buttons = b.pendingButtons;
                b.hasPending = false;
                b.idleTicks = 0;
                if (b.watchdogTripped)
                    logLine("tick %lu: bot %d input resumed", m_tick, b.id);
                b.watchdogTripped = false;
                b.levelAndHold = false;
            }
            else
            {
                b.buttons = 0;   // a button is pressed only for the tick its SetInput is applied
                if (++b.idleTicks >= static_cast<unsigned int>(m_config.watchdogTicks) && !b.watchdogTripped)
                {
                    neutralise(b, p);
                    logLine("tick %lu: bot %d watchdog: no SetInput for %u ticks; %s", m_tick, b.id, b.idleTicks,
                         b.levelAndHold ? "heli level-and-hold neutral" : "cyclic, pedals and look 0, collective held");
                }
                if (b.watchdogTripped)
                    flyNeutral(b, p);
            }
            writeInput(b, p);
            ++i;
        }
    }

    // ---------------------------------------------------------------- spawn fallback (v1)

    typedef IServerExtension::SpawnGroupInfo GroupInfo;

    // listSpawnGroups into m_groups, capped at its capacity.
    int listGroups(int team)
    {
        const int n = m_ext->listSpawnGroups(team, m_groups, kMaxSpawnGroups);
        return n < kMaxSpawnGroups ? n : kMaxSpawnGroups;
    }

    void logGroups(int team, const GroupInfo* groups, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const GroupInfo& g = groups[i];
            logLine("  team %d spawn group %d: %d points (%d enter-on-spawn), active %d, squad %d, human-only %d, pos %.1f %.1f %.1f",
                 team, g.id, g.pointCount, g.enterOnSpawnPoints, g.active ? 1 : 0, g.squad, g.onlyForHuman ? 1 : 0,
                 g.x, g.y, g.z);
        }
    }

    void resetFallback(Bot& b)
    {
        b.target = b.home;
        b.readySince = 0;
        b.targetSince = 0;
        b.lastBlocked = 0;
        b.order.clear();
        b.tried.clear();
        b.cycled = false;
    }

    void buildOrder(Bot& b, const GroupInfo* groups, int n)
    {
        b.order = spawn::fallbackOrder(b.home, b.target, groups, n);
        b.tried.assign(b.order.size(), false);
    }

    // Re-resolves home and target for b.team and restarts the fallback. On failure the bot keeps no
    // group and updateBots retries once a second; until then no fallback timer runs.
    bool resolveGroups(Bot& b, world::IPlayer* p)
    {
        const spawn::Choice c = spawn::choose(m_groups, listGroups(b.team));
        b.lastResolve = m_tick;
        b.home = c.home;
        resetFallback(b);
        b.target = c.target;
        b.groupsPending = !c.ok;
        if (c.ok)
        {
            p->setSpawnGroup(b.target);
            b.lastGroupTry = m_tick;
        }
        return c.ok;
    }

    // An admin or ModManager changed the bot's team (bf2cc switchplayer, !sw, BF2WebAdmin team,
    // mm_autobalance). Player::setTeam cleared its spawn group; the new team's groups replace the
    // old team's.
    void onTeamChanged(Bot& b, world::IPlayer* p, int team)
    {
        const int old = b.team;
        b.team = team;
        const bool ok = resolveGroups(b, p);
        logLine("tick %lu: bot %d team changed %d -> %d outside the module (%s); home spawn group %d target %d%s", m_tick,
             b.id, old, team, p->getIsAlive() ? "alive" : "dead", b.home, b.target,
             ok ? "" : "; the team has no usable spawn group, retrying every second");
        logGroups(team, m_groups, listGroups(team));
    }

    // The target's fallback time is up: move to the next group in the order that is active now.
    void advanceTarget(Bot& b, world::IPlayer* p)
    {
        const int n = listGroups(b.team);
        if (b.order.empty())
            buildOrder(b, m_groups, n);
        b.targetSince = m_tick;
        if (b.order.empty())
            return;
        const int size = static_cast<int>(b.order.size());
        int pos = -1;
        for (int i = 0; i < size; ++i)
            if (b.order[i] == b.target)
                pos = i;
        if (pos >= 0)
            b.tried[pos] = true;
        int chosen = -1;
        for (int k = 1; k <= size && chosen < 0; ++k)
        {
            const int idx = pos < 0 ? k - 1 : (pos + k) % size;
            const GroupInfo* g = spawn::findGroup(m_groups, n, b.order[idx]);
            if (g && g->active)
                chosen = idx;
            else
                b.tried[idx] = true;
        }
        bool all = true;
        for (int i = 0; i < size; ++i)
            all = all && b.tried[i];
        if (all && !b.cycled)
            logLine("tick %lu: bot %d has tried every spawn group of team %d (%d)", m_tick, b.id, b.team, size);
        b.cycled = all;
        const int previous = b.target;
        if (chosen >= 0)
            b.target = b.order[chosen];
        else if (b.target < 0)
            b.target = b.order[0];
        if (b.target != previous)
        {
            const GroupInfo* g = spawn::findGroup(m_groups, n, b.target);
            logLine("tick %lu: bot %d spawn fallback: group %d -> %d (%s)", m_tick, b.id, previous, b.target,
                 g && g->enterOnSpawnPoints > 0 ? "enter-on-spawn" : "on foot");
        }
        p->setSpawnGroup(b.target);
        b.lastGroupTry = m_tick;
    }

    void updateSpawnFallback(Bot& b, world::IPlayer* p)
    {
        if (b.groupsPending)
        {
            // No usable group for the bot's team: nothing to offer, no timers, no SpawnBlocked.
            b.readySince = 0;
            return;
        }
        if (b.target >= 0 && p->getSpawnGroup() != b.target && m_tick - b.lastGroupTry >= kGroupRetryTicks)
        {
            // uPlayingSpawning clears a group that is inactive at the moment; keep offering it.
            b.lastGroupTry = m_tick;
            p->setSpawnGroup(b.target);
        }
        // Dead players respawn only while the game is playing and their spawn delay has run out
        // (ServerGameLogic::uPlayingSpawning); the fallback timers run only then.
        const bool readyToSpawn = m_ext->getGameStatus() == kGameStatusPlaying && !(p->getTimeToSpawn() > 0.0f);
        if (!readyToSpawn)
        {
            b.readySince = 0;
            return;
        }
        if (!b.readySince)
        {
            b.readySince = m_tick;
            b.targetSince = m_tick;
        }
        if (b.target < 0 || m_tick - b.targetSince >= static_cast<unsigned long>(m_config.spawnFallbackSeconds) * 30)
            advanceTarget(b, p);
        const unsigned long waited = m_tick - b.readySince;
        if (b.cycled && waited >= static_cast<unsigned long>(m_config.spawnBlockedSeconds) * 30 &&
            (!b.lastBlocked || m_tick - b.lastBlocked >= kSpawnBlockedRepeatTicks))
        {
            b.lastBlocked = m_tick;
            const unsigned long seconds = waited / 30;
            logLine("tick %lu: bot %d SpawnBlocked: not spawned for %lu s with every spawn group tried", m_tick, b.id, seconds);
            if (ready(b.conn))
            {
                std::vector<uint8_t> f;
                proto::encodeSpawnBlocked(f, static_cast<uint8_t>(b.id),
                                          static_cast<uint16_t>(seconds > 0xFFFF ? 0xFFFF : seconds));
                control(b.conn, f);
            }
        }
    }

    void onSpawned(Bot& b, world::IPlayer* p)
    {
        IObject* root = 0;
        IPlayerControlObject* seat = seatOf(p, &root);
        const int group = p->getSpawnGroup();
        const unsigned long waited = b.readySince ? m_tick - b.readySince : 0;
        Vec3 pos;
        pos.x = pos.y = pos.z = 0.0f;
        if (root)
            pos = root->getAbsolutePosition();
        logLine("tick %lu: bot %d spawned at group %d (%s) after %lu ticks ready; %s%s seat %d; pos %.1f %.1f %.1f", m_tick,
             b.id, group, group == b.home ? "home" : "fallback", waited, seat ? "in " : "on foot",
             seat ? templateName(root).c_str() : "", seat ? seat->getPcoId() : -1, pos.x, pos.y, pos.z);
        // protocol.md v1: after a successful spawn the target goes back to the home group.
        if (b.home >= 0 && group != b.home)
            p->setSpawnGroup(b.home);
        resetFallback(b);
    }

    // Tracks a bot's camera view mode (TickState, v2 item 1) and logs its changes and the firing
    // state changes (docs/findings/module-v1.md, Q11).
    void observe(Bot& b, world::IPlayer* p)
    {
        world::ICameraObject* cam = p->getCamera();
        const int vm = cam ? static_cast<int>(cam->getViewMode()) : -1;
        if (vm != b.viewMode)
        {
            if (b.viewMode != -1)
                logLine("tick %lu: bot %d camera view mode %d -> %d", m_tick, b.id, b.viewMode, vm);
            b.viewMode = vm;
        }
        const bool manDown = p->getIsManDown();
        if (manDown != b.manDown)
        {
            b.manDown = manDown;
            logLine("tick %lu: bot %d man-down %d (time left %.1f s, time to spawn %.1f s)", m_tick, b.id, manDown ? 1 : 0,
                 p->getManDownTimeLeft(), p->getTimeToSpawn());
        }
        const bool firing = p->getFiringState();
        if (firing != b.firing)
        {
            b.firing = firing;
            if (++m_firingLogs <= 50 || m_firingLogs % 100 == 0)
                logLine("tick %lu: bot %d firing %d (change %lu)", m_tick, b.id, firing ? 1 : 0, m_firingLogs);
        }
    }

    // ---------------------------------------------------------------- vehicle set (v1)

    // The vehicle behind (id, stored), or null if the id no longer names that object. Only the
    // engine's own lookup result is dereferenced, never the stored pointer.
    IObject* liveVehicle(unsigned short id, IObject* stored)
    {
        IObject* o = m_game->getObjectFromNetworkableId(id);
        return o && o == stored && vehiclePco(o) ? o : 0;
    }

    void logVehicle(const char* what, unsigned short id, IObject* o)
    {
        bool destroyed = false;
        const float h = healthOf(o, &destroyed);
        const Vec3& p = o->getAbsolutePosition();
        logLine("tick %lu: vehicle %u '%s' %s: pos %.1f %.1f %.1f health %.2f destroyed %d objectDestroyed %d destroyList %d",
             m_tick, static_cast<unsigned>(id), templateName(o).c_str(), what,
             p.x, p.y, p.z, h, destroyed ? 1 : 0, o->isObjectDestroyed() ? 1 : 0, o->getObjectInDestroyList() ? 1 : 0);
    }

    // Walks the registered objects a slice per tick (all of them after a level unload), adding
    // vehicles to m_vehicles. Vanished vehicles are dropped when liveVehicle fails.
    void scanVehicles()
    {
        const std::map<unsigned int, IObject*>& all = m_objects->getAllRegisteredObjects();
        size_t budget = m_scanFull ? all.size() + 1 : kVehicleScanPerTick;
        std::map<unsigned int, IObject*>::const_iterator it = m_scanAtStart ? all.begin() : all.upper_bound(m_scanKey);
        m_scanAtStart = false;
        for (; it != all.end() && budget; ++it, --budget)
        {
            m_scanKey = it->first;
            IObject* o = it->second;
            if (!vehiclePco(o))
                continue;
            io::INetworkable* net = o->getNetworkable();
            if (!net || !net->m_networkableId)
                continue;
            IObject*& slot = m_vehicles[net->m_networkableId];
            if (slot != o)
                logVehicle("added to the vehicle set", net->m_networkableId, o);
            slot = o;
        }
        if (it == all.end())
        {
            m_scanAtStart = true;
            if (m_scanFull)
                logLine("tick %lu: vehicle set rebuilt: %u vehicles in %u registered objects", m_tick,
                     static_cast<unsigned>(m_vehicles.size()), static_cast<unsigned>(all.size()));
        }
        m_scanFull = false;
    }

    // Builds the VehicleList once and sends it to every ready connection that is due.
    void sendVehicleList()
    {
        bool due = false;
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
            due = due || (c->second.ready && m_tick >= c->second.vehicleListDue);
        if (!due)
            return;
        m_vehicleEntries.clear();
        for (std::map<unsigned short, IObject*>::iterator it = m_vehicles.begin(); it != m_vehicles.end();)
        {
            IObject* o = liveVehicle(it->first, it->second);
            if (!o)
            {
                logLine("tick %lu: vehicle %u left the vehicle set", m_tick, static_cast<unsigned>(it->first));
                m_vehicles.erase(it++);
                continue;
            }
            IPlayerControlObject* pco = vehiclePco(o);
            m_vehicleEntries.push_back(proto::VehicleEntry());
            proto::VehicleEntry& e = m_vehicleEntries.back();
            e.netId = it->first;
            e.templateName = templateName(o);
            const int team = pco->getTeam();
            e.team = static_cast<uint8_t>(team > 0 && team < 256 ? team : 0);
            bool destroyed = false;
            e.health = healthOf(o, &destroyed);
            e.flags = destroyed ? proto::kVehicleDestroyed : 0;
            const Mat4& m = o->getAbsoluteTransformation();
            e.pos[0] = m.m[12];
            e.pos[1] = m.m[13];
            e.pos[2] = m.m[14];
            e.yaw = yawOf(m);
            e.seats = 0;
            e.occupied = 0;
            std::map<int, IPlayerControlObject*>* seats = pco->getPcos();
            if (seats)
            {
                e.seats = static_cast<uint8_t>(seats->size() > 255 ? 255 : seats->size());
                for (std::map<int, IPlayerControlObject*>::iterator s = seats->begin(); s != seats->end(); ++s)
                    if (s->first >= 0 && s->first < 32 && s->second && s->second->isOccupied())
                        e.occupied |= 1u << s->first;
            }
            ++it;
        }
        std::vector<uint8_t> encoded;
        proto::encodeVehicleList(encoded, static_cast<uint32_t>(m_tick - 1), m_vehicleEntries);
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            if (!c->second.ready || m_tick < c->second.vehicleListDue)
                continue;
            c->second.vehicleListDue = m_tick + kVehicleListPeriod;
            std::vector<uint8_t> f(encoded);
            g_channel->sendVehicles(c->first, f);
        }
    }

    // ---------------------------------------------------------------- projectiles (v2 item 5)

    void sendProjectiles()
    {
        // Drop what is no longer in flight. Only the engine's own lookup result is dereferenced.
        m_live.clear();
        for (size_t i = 0; i < m_tracked.size();)
        {
            Tracked& t = m_tracked[i];
            IObject* o = m_objects->getRegisteredObject(static_cast<int>(t.objectId));
            IServerExtension::ProjectileInfo info;
            if (o != t.object || (o->getFlags() & kCachedObjectFlag) || !m_ext->getProjectileInfo(o, info) || !info.active)
            {
                m_tracked.erase(m_tracked.begin() + i);
                continue;
            }
            m_live.push_back(proto::ProjectileEntry());
            proto::ProjectileEntry& e = m_live.back();
            e.id = t.wireId;
            e.templateName = t.templateName;
            const bool hasOwner = info.ownerPlayerId >= 0 && info.ownerPlayerId <= 255;
            e.owner = hasOwner ? static_cast<uint8_t>(info.ownerPlayerId) : 0;
            e.team = static_cast<uint8_t>(info.team > 0 && info.team < 256 ? info.team : 0);
            e.flags = (t.guided ? proto::kProjectileGuided : 0) | (hasOwner ? proto::kProjectileHasOwner : 0);
            e.targetId = netIdOf(rootOf(info.target));
            const Vec3& pos = o->getAbsolutePosition();
            e.pos[0] = pos.x; e.pos[1] = pos.y; e.pos[2] = pos.z;
            IPhysicsNode* node = static_cast<IPhysicsNode*>(o->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
            Vec3 v;
            v.x = v.y = v.z = 0.0f;
            if (node)
                v = node->getPositionalSpeed();
            e.vel[0] = v.x; e.vel[1] = v.y; e.vel[2] = v.z;
            e.timeToLive = info.timeToLive;
            ++i;
        }

        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            Conn& conn = c->second;
            if (!conn.ready)
                continue;
            // The connection's live bots' root positions, for the range test.
            m_botPositions.clear();
            for (size_t i = 0; i < m_bots.size(); ++i)
            {
                world::IPlayer* p = m_bots[i].conn == c->first && m_bots[i].removeCountdown < 0 ? live(m_bots[i]) : 0;
                IObject* root = p && p->getIsAlive() ? rootOf(p->getVehicle()) : 0;
                if (root)
                    m_botPositions.push_back(root->getAbsolutePosition());
            }
            m_projectileOut.clear();
            for (size_t i = 0; i < m_live.size() && m_projectileOut.size() < proto::kMaxProjectiles; ++i)
            {
                const proto::ProjectileEntry& e = m_live[i];
                world::IPlayer* owner = e.flags & proto::kProjectileHasOwner ? m_players->getPlayerFromId(e.owner) : 0;
                const bool own = owner && isOwnBot(owner, c->first);
                bool inRange = own;
                Vec3 at;
                at.x = e.pos[0]; at.y = e.pos[1]; at.z = e.pos[2];
                for (size_t k = 0; k < m_botPositions.size() && !inRange; ++k)
                    inRange = distance(m_botPositions[k], at) <= m_config.projectileRange;
                if (!inRange)
                    continue;
                m_projectileOut.push_back(e);
                if (own)
                    m_projectileOut.back().flags |= proto::kProjectileOwn;
            }
            if (m_projectileOut.empty() && !conn.projectilesShown)
                continue;
            conn.projectilesShown = !m_projectileOut.empty();
            std::vector<uint8_t> f;
            proto::encodeProjectileList(f, static_cast<uint32_t>(m_tick - 1), m_projectileOut);
            g_channel->sendLatest(c->first, Channel::SlotProjectiles, f);
        }
    }

    // ---------------------------------------------------------------- round state (v2 item 7)

    void sendRoundState()
    {
        if (m_conns.empty())
            return;
        const int status = m_ext->getGameStatus();
        const int brs = m_ext->getBeginRoundState();
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            Conn& conn = c->second;
            if (!conn.ready || (conn.lastStatus == status && conn.lastBeginRound == brs))
                continue;
            if (conn.lastStatus != -2)
                logLine("tick %lu: round state: status %d -> %d, begin-round state %d -> %d (connection %u)", m_tick,
                     conn.lastStatus, status, conn.lastBeginRound, brs, c->first);
            conn.lastStatus = status;
            conn.lastBeginRound = brs;
            std::vector<uint8_t> f;
            proto::encodeRoundState(f, static_cast<uint32_t>(m_tick - 1), static_cast<uint8_t>(status >= 0 && status < 255 ? status : 255),
                                    static_cast<uint8_t>(brs >= 0 && brs < 255 ? brs : 255), m_ext->getTimeToNextStateChange());
            control(c->first, f);
        }
    }

    // ---------------------------------------------------------------- state

    void pruneSpotted()
    {
        for (std::map<unsigned short, Spotted>::iterator s = m_spotted.begin(); s != m_spotted.end();)
        {
            bool current = false;
            for (int t = 0; t < 4; ++t)
                current = current || s->second.until[t] > m_tick;
            if (current)
                ++s;
            else
                m_spotted.erase(s++);
        }
    }

    bool spotted(uint32_t rootId, int team) const
    {
        if (!rootId)
            return false;
        std::map<unsigned short, Spotted>::const_iterator s = m_spotted.find(static_cast<unsigned short>(rootId));
        if (s == m_spotted.end())
            return false;
        for (int t = 0; t < 4; ++t)
            if (t != team && s->second.until[t] > m_tick)
                return true;
        return false;
    }

    void fillEntry(world::IPlayer* p, uint8_t flags, proto::StateEntry& e)
    {
        e.playerId = static_cast<uint8_t>(p->getId());
        e.team = static_cast<uint8_t>(p->getTeam());
        e.seat = proto::kSeatOnFoot;
        e.rootId = 0;
        for (int k = 0; k < 3; ++k)
            e.pos[k] = e.ypr[k] = e.vel[k] = e.angVel[k] = 0.0f;
        e.health = -1.0f;
        e.viewMode = proto::kViewNone;
        e.weaponCount = 0;

        IObject* root = 0;
        IPlayerControlObject* seat = seatOf(p, &root);
        const bool alive = p->getIsAlive();
        if (alive)
            flags |= proto::kFlagAlive;
        if (alive && p->getIsManDown())
            flags |= proto::kFlagManDown;
        if (seat)
        {
            flags |= proto::kFlagInVehicle;
            e.seat = static_cast<uint8_t>(seat->getPcoId());
        }
        e.flags = flags;
        if (!alive || !root)
            return;

        e.rootId = netIdOf(root);
        if (spotted(e.rootId, p->getTeam()))
            e.flags |= proto::kFlagSpotted;
        const Mat4& m = root->getAbsoluteTransformation();
        e.pos[0] = m.m[12];
        e.pos[1] = m.m[13];
        e.pos[2] = m.m[14];
        attitudeOf(m, e.ypr);
        IPhysicsNode* node = static_cast<IPhysicsNode*>(root->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
        if (node)
        {
            const Vec3 v = node->getPositionalSpeed();
            const Vec3 w = node->getRotationalSpeed();
            e.vel[0] = v.x; e.vel[1] = v.y; e.vel[2] = v.z;
            e.angVel[0] = w.x; e.angVel[1] = w.y; e.angVel[2] = w.z;
        }
        if (seat)
        {
            IArmorGLComp* armor = static_cast<IArmorGLComp*>(root->queryComponent(IID_IArmorGLComp, IID_IArmorGLComp));
            if (armor)
            {
                const float maxHp = armor->getMaxHitPoints();
                float h = maxHp > 0.0f ? armor->getHitPoints() / maxHp : 0.0f;
                if (!(h >= 0.0f)) h = 0.0f;
                if (h > 1.0f) h = 1.0f;
                e.health = h;
            }
        }
    }

    // v2 items 1 and 4: the own bot's camera view mode and the weapons in its active slots.
    void fillOwnExtras(const Bot& b, world::IPlayer* p, proto::StateEntry& e)
    {
        if (!p->getIsAlive())
            return;
        e.viewMode = b.viewMode >= 0 && b.viewMode < 255 ? static_cast<uint8_t>(b.viewMode) : 0;
        IObject* holder = p->getVehicle();
        IPlayerControlObject* pco =
            holder ? static_cast<IPlayerControlObject*>(holder->queryInterface(IID_IPlayerControlObject)) : 0;
        if (!pco || holder->queryInterface(IID_ICameraObject))
            return;
        for (int slot = 0; slot < proto::kMaxWeapons; ++slot)
        {
            IObject* weapon = pco->getActiveWeapon(slot);
            IWeaponObject* w = weapon ? static_cast<IWeaponObject*>(weapon->queryInterface(IID_IWeaponObject)) : 0;
            if (!w)
                continue;
            proto::WeaponState& ws = e.weapons[e.weaponCount++];
            ws.slot = static_cast<uint8_t>(slot);
            ws.templateName = templateName(weapon);
            const int ammo = w->getAmmoInMag(0);
            const int mags = w->getCurrentNrOfMags();
            ws.ammoInMag = static_cast<int16_t>(ammo < -1 ? -1 : ammo > 32767 ? 32767 : ammo);
            ws.mags = static_cast<int16_t>(mags < -1 ? -1 : mags > 32767 ? 32767 : mags);
            const bool reloading = w->isReloading();
            ws.flags = reloading ? proto::kWeaponReloading : 0;
            float left = reloading ? w->getReloadTimeLeftInPercent() : 0.0f;
            if (!(left >= 0.0f)) left = 0.0f;
            if (left > 1.0f) left = 1.0f;
            ws.reloadLeft = left;
            ws.heat = w->getUsesHeat() ? w->getHeat() : -1.0f;
        }
    }

    void sendState()
    {
        if (m_tick % kSpottedTicks == 0)
            pruneSpotted();
        if (m_conns.empty())
            return;
        // Other players in vehicles, filled once and shared by every connection.
        m_vehicleOccupants.clear();
        const std::list<world::IPlayer*>& players = m_players->getPlayers();
        for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
        {
            world::IPlayer* p = *it;
            if (!p || !seatOf(p, 0))
                continue;
            m_vehicleOccupants.push_back(std::make_pair(p, proto::StateEntry()));
            fillEntry(p, 0, m_vehicleOccupants.back().second);
        }
        const double time = m_system ? m_system->getExactTime() : (m_tick - 1) / static_cast<double>(kTickRate);
        for (std::map<uint32_t, Conn>::iterator c = m_conns.begin(); c != m_conns.end(); ++c)
        {
            if (!c->second.ready)
                continue;
            m_entries.clear();
            for (size_t i = 0; i < m_bots.size(); ++i)
            {
                const Bot& b = m_bots[i];
                if (b.conn != c->first || b.removeCountdown >= 0)
                    continue;
                world::IPlayer* p = live(b);
                if (!p)
                    continue;
                m_entries.push_back(proto::StateEntry());
                fillEntry(p, proto::kFlagOwnBot, m_entries.back());
                fillOwnExtras(b, p, m_entries.back());
            }
            for (size_t i = 0; i < m_vehicleOccupants.size(); ++i)
                if (!isOwnBot(m_vehicleOccupants[i].first, c->first))
                    m_entries.push_back(m_vehicleOccupants[i].second);
            m_stateBuf.clear();
            // The snapshot is the end of the previous simulation tick (Q1).
            proto::encodeTickState(m_stateBuf, static_cast<uint32_t>(m_tick - 1), time, m_entries);
            g_channel->sendState(c->first, m_stateBuf);
        }
    }

    Config m_config;
    EventManager* m_em;
    IServerExtension* m_ext;
    PlayerManager* m_players;
    ObjectManager* m_objects;
    IGame* m_game;
    IGameServer* m_server;
    System* m_system;

    unsigned long m_tick;
    std::map<uint32_t, Conn> m_conns;
    bool m_unloadSeen;
    unsigned long long m_mask;
    unsigned long m_rejectedInputs;
    unsigned long m_lastRejectLog;
    unsigned int m_scanKey;
    bool m_scanFull;
    bool m_scanAtStart = true;
    unsigned long m_lastRestart;
    bool m_restarted;
    unsigned long m_firingLogs = 0;
    unsigned long m_projectileLogs = 0;
    unsigned long m_skippedLogs = 0;
    unsigned long m_launches = 0;
    unsigned long m_spotLogs = 0;
    unsigned long m_chatLogs = 0;
    GroupInfo m_groups[kMaxSpawnGroups];
    std::map<unsigned short, IObject*> m_vehicles;   // networkable id -> root; validated on every use
    std::vector<proto::VehicleEntry> m_vehicleEntries;
    std::vector<Tracked> m_tracked;
    std::vector<proto::ProjectileEntry> m_live;
    std::vector<proto::ProjectileEntry> m_projectileOut;
    std::vector<Vec3> m_botPositions;
    std::map<unsigned short, Spotted> m_spotted;     // spotted object's networkable id -> expiry per team

    std::vector<Bot> m_bots;
    std::deque<Command> m_inbox;
    std::vector<std::pair<world::IPlayer*, proto::StateEntry> > m_vehicleOccupants;
    std::vector<proto::StateEntry> m_entries;
    std::vector<uint8_t> m_stateBuf;
    CostStats m_cost;
};

} // namespace

extern "C" __attribute__((visibility("default"))) bool initDll(ClassManager* classManager)
{
    if (!classManager)
        return false;
    // Never destroyed: the engine keeps the listener and observer pointers and the loader has no
    // unload path.
    Module* module = new Module();
    if (!module->init(classManager))
    {
        delete module;
        return false;
    }
    return true;
}
