// libserverext: the engine side of the bot channel, docs/protocol.md v1.
//
// Everything in this file runs on the server's main thread, inside the EventManager category-7
// event that the module re-posts every tick (Q1), plus the ECHud unload notification. Socket work
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
#include "Game/IEventListener.h"
#include "Game/IGame.h"
#include "Game/IGameServer.h"
#include "Game/Objects/ICameraObject.h"
#include "Game/GameServer/IServerExtension.h"
#include "Game/Objects/GameLogicComp/IArmorGLComp.h"
#include "Game/Objects/IObject.h"
#include "Game/Objects/IObjectTemplate.h"
#include "Game/Objects/IPlayer.h"
#include "IO/Input/PlayerInput.h"
#include "IO/Network/INetworkable.h"
#include "Physics/IPhysicsNode.h"
#include "Scene/Object/ObjectManager.h"
#include "Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#include "Scene/Object/PlayerManager.h"

#include "Channel.h"
#include "Config.h"
#include "IoThread.h"
#include "Protocol.h"

using namespace dice::hfe;
using namespace dice::hfe::world;
using namespace serverext;

namespace {

const EventCategory kTickCategory = static_cast<EventCategory>(7);   // [CHOSEN] Q1/Q8
const unsigned int kTickEventId = 1;
const unsigned int kUnloadHudEventId = 3;   // GameServer::unload posts (ECHud, 3) first
const float kTickRate = 30.0f;
const int kDefaultKit = 0;
const unsigned int kStatsPeriodTicks = 1800;   // one stats line per minute
const unsigned int kGroupRetryTicks = 10;      // re-set a cleared target spawn group this often
const unsigned int kVehicleListPeriod = 30;     // protocol.md v1: once a second
const size_t kVehicleScanPerTick = 500;         // registered objects examined per tick
const unsigned int kRestartMinTicks = 60 * 30;  // protocol.md v1: at most one RestartRound per 60 s
const unsigned int kSpawnBlockedRepeatTicks = 10 * 30;
const int kMaxSpawnGroups = 64;
const int kGameStatusPlaying = 1;               // the only status in which dead players respawn

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

    void report(unsigned long tick, size_t bots)
    {
        logLine("tick %lu cost (handler, us): last %u ticks p50 %.0f p99 %.0f max %.0f | run %llu ticks p50 %.0f p99 %.0f max %.0f (>20ms: %llu) | bots %u",
             tick, m_windowCount, pct(m_window, m_windowCount, 0.50), pct(m_window, m_windowCount, 0.99), m_windowMax,
             static_cast<unsigned long long>(m_total), pct(m_all, m_total, 0.50), pct(m_all, m_total, 0.99), m_maxUs,
             static_cast<unsigned long long>(m_overflow), static_cast<unsigned>(bots));
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
    int team;
    float axes[proto::AxisCount];      // what is written every tick
    float pending[proto::AxisCount];   // latest SetInput of this tick
    uint32_t buttons;                  // pressed for this tick only
    uint32_t pendingButtons;
    bool hasPending;
    unsigned int idleTicks;
    bool watchdogTripped;
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

    int viewMode;                 // last camera view mode seen, -1 if none (test observability)
    bool firing;
};

class Module : public IEventListener
{
public:
    Module()
        : m_em(0), m_ext(0), m_players(0), m_objects(0), m_game(0), m_server(0), m_system(0),
          m_tick(0), m_conn(0), m_ready(false), m_unloadSeen(false), m_mask(humanMappingMask()),
          m_rejectedInputs(0), m_lastRejectLog(0), m_scanKey(0), m_scanFull(true), m_vehicleListDue(0),
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
        const unsigned int handle = m_em->postEventNextUpdate(kTickCategory, kTickEventId, 0);
        logLine("loaded: protocol v%u, bot channel %s:%d, watchdog %d ticks, enterRange %.1f m, spawnFallback %d s, "
             "spawnBlocked %d s, allowRoundRestart %d; tick hook %d (event %u), unload hook %d, System %p",
             static_cast<unsigned>(proto::kVersion), m_config.listenHost.c_str(), m_config.listenPort,
             m_config.watchdogTicks, m_config.enterRange, m_config.spawnFallbackSeconds, m_config.spawnBlockedSeconds,
             m_config.allowRoundRestart ? 1 : 0, tickOk ? 1 : 0, handle, hudOk ? 1 : 0, (void*)m_system);
        // The listener is already up, so the module cannot back out once the hook is registered.
        return true;
    }

    virtual void handleEvent(EventCategory category, unsigned int id, EventNode*, void*)
    {
        if (category == ECHud)
        {
            if (id == kUnloadHudEventId)
            {
                m_unloadSeen = true;
                m_vehicles.clear();
                m_scanFull = true;   // rebuilt with one full walk on the next tick
                logLine("tick %lu: level unload", m_tick);
            }
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
        sendVehicleList();
        m_unloadSeen = m_unloadSeen && hasMissingBots();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        m_cost.add(us);
        if (m_tick % kStatsPeriodTicks == 0)
            m_cost.report(m_tick, m_bots.size());
    }

private:
    // ---------------------------------------------------------------- outbound helpers

    void control(std::vector<uint8_t>& frame) { g_channel->sendControl(m_conn, frame); }

    void error(uint32_t requestId, uint16_t code, const std::string& message)
    {
        std::vector<uint8_t> f;
        proto::encodeError(f, requestId, code, message);
        control(f);
    }

    void ack(uint32_t requestId)
    {
        std::vector<uint8_t> f;
        proto::encodeAck(f, requestId);
        control(f);
    }

    void botRemoved(const Bot& b, uint8_t reason)
    {
        if (b.conn != m_conn || !m_ready)
            return;
        std::vector<uint8_t> f;
        proto::encodeBotRemoved(f, static_cast<uint8_t>(b.id), reason);
        control(f);
    }

    // ---------------------------------------------------------------- bot lookup

    world::IPlayer* live(const Bot& b) const
    {
        world::IPlayer* p = m_players->getPlayerFromId(b.id);
        return p == b.player ? p : 0;
    }

    Bot* ownBot(int id)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].id == id && m_bots[i].conn == m_conn && m_bots[i].removeCountdown < 0)
                return &m_bots[i];
        return 0;
    }

    bool hasMissingBots() const
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].missingTicks > 0)
                return true;
        return false;
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
                m_conn = c.conn;
                m_ready = false;
                continue;
            }
            if (c.kind == Command::Disconnected)
            {
                if (c.conn == m_conn)
                    onDisconnect();
                continue;
            }
            if (c.conn != m_conn)
                continue;   // left over from a closed connection
            switch (c.kind)
            {
            case Command::Hello:        onHello(c.hello); break;
            case Command::CreateBot:    onCreateBot(c.createBot); break;
            case Command::EnterVehicle: onEnterVehicle(c.enterVehicle); break;
            case Command::RemoveBot:    onRemoveBot(c.removeBot); break;
            case Command::SetInput:     onSetInput(c.setInput); break;
            case Command::RestartRound: onRestartRound(c.restartRound); break;
            default: break;
            }
        }
        m_inbox.clear();
    }

    void onDisconnect()
    {
        size_t n = 0;
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            Bot& b = m_bots[i];
            if (b.conn != m_conn || b.removeCountdown >= 0)
                continue;
            b.hasPending = false;
            neutralise(b, live(b));
            b.removeCountdown = m_config.watchdogTicks;
            ++n;
        }
        logLine("tick %lu: sidecar connection %u gone; %u bots neutralised, removal in %d ticks",
             m_tick, m_conn, static_cast<unsigned>(n), m_config.watchdogTicks);
        m_conn = 0;
        m_ready = false;
    }

    void onHello(const proto::HelloMsg& m)
    {
        const std::string map = m_game->getCurrentMapName();
        const std::string mode = m_game->getCurrentMapGameMode();
        std::vector<uint8_t> f;
        proto::encodeWelcome(f, kTickRate, static_cast<uint32_t>(m_tick), map, mode);
        control(f);
        m_ready = true;
        m_vehicleListDue = m_tick + 1;   // the first tick after Welcome
        logLine("tick %lu: Hello from '%s' on connection %u; Welcome map '%s' mode '%s'", m_tick,
             sanitizeName(m.name, 64).c_str(), m_conn, map.c_str(), mode.c_str());
    }

    void onCreateBot(const proto::CreateBotMsg& m)
    {
        if (m.team != 1 && m.team != 2)
        {
            error(m.requestId, proto::ErrInvalidValue, "team must be 1 or 2");
            return;
        }
        const std::string name = sanitizeName(m.name, 64);
        if (name.empty())
        {
            error(m.requestId, proto::ErrInvalidValue, "name is empty");
            return;
        }
        // protocol.md v1: CreateBot succeeds whenever the team has a spawn group a bot can use,
        // active or not; a blocked one is handled by the spawn fallback.
        const int groupCount = m_ext->listSpawnGroups(m.team, m_groups, kMaxSpawnGroups);
        const int group = homeGroup(m_groups, groupCount < kMaxSpawnGroups ? groupCount : kMaxSpawnGroups);
        if (group < 0 && !anyUsableGroup(m_groups, groupCount < kMaxSpawnGroups ? groupCount : kMaxSpawnGroups))
        {
            error(m.requestId, proto::ErrRefused, "the team has no spawn group (is a level loaded?)");
            logLine("tick %lu: CreateBot '%s' team %d refused: %d spawn groups, none usable", m_tick, name.c_str(),
                 m.team, groupCount);
            return;
        }
        const int id = m_ext->createInputPlayer(name);
        world::IPlayer* p = id >= 0 ? m_players->getPlayerFromId(id) : 0;
        if (!p || id > 255)
        {
            error(m.requestId, proto::ErrRefused, "the engine refused to create the player (player limit?)");
            logLine("tick %lu: CreateBot '%s' refused: createInputPlayer -> %d", m_tick, name.c_str(), id);
            return;
        }
        p->setTeam(m.team, false);
        p->setKit(kDefaultKit);

        Bot b;
        b.id = id;
        b.player = p;
        b.conn = m_conn;
        b.team = m.team;
        for (int i = 0; i < proto::AxisCount; ++i)
            b.axes[i] = b.pending[i] = 0.0f;
        b.buttons = b.pendingButtons = 0;
        b.hasPending = false;
        b.idleTicks = 0;
        b.watchdogTripped = false;
        b.wasAlive = p->getIsAlive();
        b.removeCountdown = -1;
        b.missingTicks = 0;
        b.lastGroupTry = m_tick;
        b.home = group;
        b.viewMode = -1;
        b.firing = false;
        resetFallback(b);
        if (b.target < 0)
            b.target = firstFallbackGroup(m_groups, groupCount < kMaxSpawnGroups ? groupCount : kMaxSpawnGroups);
        p->setSpawnGroup(b.target);
        m_bots.push_back(b);

        std::vector<uint8_t> f;
        proto::encodeBotCreated(f, m.requestId, static_cast<uint8_t>(id));
        control(f);
        logLine("tick %lu: created bot %d '%s' team %d kit %d home spawn group %d target %d (ai %d connected %d)", m_tick, id,
             p->getName().c_str(), m.team, kDefaultKit, group, m_bots.back().target, p->getIsAIPlayer() ? 1 : 0,
             p->getIsConnected() ? 1 : 0);
        logGroups(m.team, m_groups, groupCount < kMaxSpawnGroups ? groupCount : kMaxSpawnGroups);
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

    void onEnterVehicle(const proto::EnterVehicleMsg& m)
    {
        Bot* b = ownBot(m.playerId);
        world::IPlayer* p = b ? live(*b) : 0;
        if (!p)
        {
            error(m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
            return;
        }
        IObject* me = rootOf(p->getVehicle());
        if (!p->getIsAlive() || !me || !me->queryInterface(IID_ISoldier))
        {
            error(m.requestId, proto::ErrRefused, "the bot must be alive and on foot");
            return;
        }

        // protocol.md v1: both forms skip destroyed vehicles and require the bot within enterRange.
        const Vec3& myPos = me->getAbsolutePosition();
        const char* why = "no undestroyed vehicle with that seat free within enterRange";
        IObject* seat = 0;
        if (m.vehicleId != 0)
        {
            if (m.vehicleId > 0xFFFF)
            {
                error(m.requestId, proto::ErrUnavailable, "vehicle id out of range");
                return;
            }
            const unsigned short netId = static_cast<unsigned short>(m.vehicleId);
            IObject* root = rootOf(m_game->getObjectFromNetworkableId(netId));
            if (!root)
            {
                error(m.requestId, proto::ErrUnavailable, "no object with that networkable id");
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
                seat = freeSeat(root, m.seat, b->team, &why);
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
                IObject* s = freeSeat(o, m.seat, b->team, &ignored);
                if (s)
                {
                    best = d;
                    seat = s;
                }
            }
        }
        if (!seat)
        {
            error(m.requestId, proto::ErrUnavailable, why);
            logLine("tick %lu: EnterVehicle bot %d vehicle %u seat %u refused: %s", m_tick, b->id, m.vehicleId, m.seat, why);
            return;
        }

        const bool ok = m_game->enterVehicle(p, seat, false);
        if (!ok || p->getVehicle() != seat)
        {
            error(m.requestId, proto::ErrRefused, "IGame::enterVehicle did not seat the bot");
            logLine("tick %lu: EnterVehicle bot %d: enterVehicle -> %d, vehicle %p, wanted %p", m_tick, b->id, ok ? 1 : 0,
                 (void*)p->getVehicle(), (void*)seat);
            return;
        }
        IObject* root = rootOf(seat);
        io::INetworkable* net = root->getNetworkable();
        logLine("tick %lu: bot %d entered '%s' (net id %u) seat %u", m_tick, b->id,
             root->getTemplate() ? root->getTemplate()->getName().c_str() : "?", net ? net->m_networkableId : 0, m.seat);
        ack(m.requestId);
    }

    void onRemoveBot(const proto::RemoveBotMsg& m)
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            Bot& b = m_bots[i];
            if (b.id != m.playerId || b.conn != m_conn || b.removeCountdown >= 0)
                continue;
            world::IPlayer* p = live(b);
            const Bot copy = b;
            m_bots.erase(m_bots.begin() + i);
            if (p)
                m_server->destroyPlayer(p);
            logLine("tick %lu: bot %d removed on request", m_tick, copy.id);
            ack(m.requestId);
            botRemoved(copy, proto::ReasonRemoveBot);
            return;
        }
        error(m.requestId, proto::ErrUnknownPlayer, "not a bot of this connection");
    }

    void onSetInput(const proto::SetInputMsg& m)
    {
        Bot* b = ownBot(m.playerId);
        if (!b)
        {
            error(0, proto::ErrUnknownPlayer, "SetInput for a player that is not a bot of this connection");
            return;
        }
        float axes[proto::AxisCount];
        for (int i = 0; i < proto::AxisCount; ++i)
        {
            if (!proto::conditionAxis(m.axes[i], axes[i]))
            {
                error(0, proto::ErrInvalidValue, "SetInput axis is NaN or infinite; input not applied");
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

    void onRestartRound(const proto::RestartRoundMsg& m)
    {
        if (!m_config.allowRoundRestart)
        {
            error(m.requestId, proto::ErrRefused, "round restart is disabled (serverext.cfg allowRoundRestart 0)");
            logLine("tick %lu: RestartRound refused: allowRoundRestart is 0", m_tick);
            return;
        }
        if (m_restarted && m_tick - m_lastRestart < kRestartMinTicks)
        {
            error(m.requestId, proto::ErrRefused, "at most one round restart per 60 s");
            logLine("tick %lu: RestartRound refused: last restart %lu ticks ago", m_tick, m_tick - m_lastRestart);
            return;
        }
        if (!m_ext->restartRound())
        {
            error(m.requestId, proto::ErrRefused, "the engine refused the round restart (no game logic)");
            logLine("tick %lu: RestartRound refused by the engine", m_tick);
            return;
        }
        m_restarted = true;
        m_lastRestart = m_tick;
        logLine("tick %lu: round restarted on request (admin.restartMap)", m_tick);
        ack(m.requestId);
    }

    // ---------------------------------------------------------------- per-tick bot work

    // Watchdog neutral: cyclic, pedals and look 0, collective held while in a vehicle. On foot every
    // axis is 0, since a held collective would keep the soldier walking. No buttons.
    void neutralise(Bot& b, world::IPlayer* p)
    {
        const bool inVehicle = p && seatOf(p, 0);
        const float collective = b.axes[proto::AxisCollective];
        for (int k = 0; k < proto::AxisCount; ++k)
            b.axes[k] = 0.0f;
        if (inVehicle)
            b.axes[proto::AxisCollective] = collective;
        b.buttons = 0;
        b.watchdogTripped = true;
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
                writeInput(b, p);
                ++i;
                continue;
            }

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
            }
            else
            {
                b.buttons = 0;   // a button is pressed only for the tick its SetInput is applied
                if (++b.idleTicks >= static_cast<unsigned int>(m_config.watchdogTicks) && !b.watchdogTripped)
                {
                    neutralise(b, p);
                    logLine("tick %lu: bot %d watchdog: no SetInput for %u ticks; cyclic, pedals and look 0, collective %.2f",
                         m_tick, b.id, b.idleTicks, b.axes[proto::AxisCollective]);
                }
            }
            writeInput(b, p);
            ++i;
        }
    }

    // ---------------------------------------------------------------- spawn fallback (v1)

    typedef IServerExtension::SpawnGroupInfo GroupInfo;

    static bool usable(const GroupInfo& g)
    {
        return g.squad <= 0 && !g.onlyForHuman && g.pointCount > 0;
    }

    // The pad: the team's lowest-id usable group without enter-on-spawn points, active or not.
    // Matches findSpawnGroup(team, false) whenever nothing is blocked.
    static int homeGroup(const GroupInfo* groups, int n)
    {
        for (int i = 0; i < n; ++i)
            if (usable(groups[i]) && groups[i].enterOnSpawnPoints == 0)
                return groups[i].id;
        return -1;
    }

    static bool anyUsableGroup(const GroupInfo* groups, int n)
    {
        for (int i = 0; i < n; ++i)
            if (usable(groups[i]))
                return true;
        return false;
    }

    static int firstFallbackGroup(const GroupInfo* groups, int n)
    {
        for (int i = 0; i < n; ++i)
            if (usable(groups[i]))
                return groups[i].id;
        return -1;
    }

    static const GroupInfo* findGroup(const GroupInfo* groups, int n, int id)
    {
        for (int i = 0; i < n; ++i)
            if (groups[i].id == id)
                return &groups[i];
        return 0;
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

    // Home first, then the other on-foot groups nearest to home first, then the groups with any
    // enter-on-spawn point, nearest first.
    void buildOrder(Bot& b, const GroupInfo* groups, int n)
    {
        const GroupInfo* ref = findGroup(groups, n, b.home >= 0 ? b.home : b.target);
        std::vector<std::pair<float, int> > onFoot, enters;
        for (int i = 0; i < n; ++i)
        {
            const GroupInfo& g = groups[i];
            if (!usable(g) || g.id == b.home)
                continue;
            float d = 0.0f;
            if (ref)
            {
                const float dx = g.x - ref->x, dy = g.y - ref->y, dz = g.z - ref->z;
                d = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            (g.enterOnSpawnPoints > 0 ? enters : onFoot).push_back(std::make_pair(d, g.id));
        }
        std::stable_sort(onFoot.begin(), onFoot.end());
        std::stable_sort(enters.begin(), enters.end());
        b.order.clear();
        if (b.home >= 0)
            b.order.push_back(b.home);
        for (size_t i = 0; i < onFoot.size(); ++i)
            b.order.push_back(onFoot[i].second);
        for (size_t i = 0; i < enters.size(); ++i)
            b.order.push_back(enters[i].second);
        b.tried.assign(b.order.size(), false);
    }

    // The target's fallback time is up: move to the next group in the order that is active now.
    void advanceTarget(Bot& b, world::IPlayer* p)
    {
        int n = m_ext->listSpawnGroups(b.team, m_groups, kMaxSpawnGroups);
        if (n > kMaxSpawnGroups)
            n = kMaxSpawnGroups;
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
            const GroupInfo* g = findGroup(m_groups, n, b.order[idx]);
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
            const GroupInfo* g = findGroup(m_groups, n, b.target);
            logLine("tick %lu: bot %d spawn fallback: group %d -> %d (%s)", m_tick, b.id, previous, b.target,
                 g && g->enterOnSpawnPoints > 0 ? "enter-on-spawn" : "on foot");
        }
        p->setSpawnGroup(b.target);
        b.lastGroupTry = m_tick;
    }

    void updateSpawnFallback(Bot& b, world::IPlayer* p)
    {
        if (b.target >= 0 && p->getSpawnGroup() != b.target && m_tick - b.lastGroupTry >= kGroupRetryTicks)
        {
            // uPlayingSpawning clears a group that is inactive at the moment; keep offering it.
            b.lastGroupTry = m_tick;
            p->setSpawnGroup(b.target);
        }
        // Dead players respawn only while the game is playing and their spawn delay has run out
        // (ServerGameLogic::uPlayingSpawning); the fallback timers run only then.
        const bool ready = m_ext->getGameStatus() == kGameStatusPlaying && !(p->getTimeToSpawn() > 0.0f);
        if (!ready)
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
            if (b.conn == m_conn && m_ready)
            {
                std::vector<uint8_t> f;
                proto::encodeSpawnBlocked(f, static_cast<uint8_t>(b.id),
                                          static_cast<uint16_t>(seconds > 0xFFFF ? 0xFFFF : seconds));
                control(f);
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
             seat && root && root->getTemplate() ? root->getTemplate()->getName().c_str() : "",
             seat ? seat->getPcoId() : -1, pos.x, pos.y, pos.z);
        // protocol.md v1: after a successful spawn the target goes back to the home group.
        if (b.home >= 0 && group != b.home)
            p->setSpawnGroup(b.home);
        resetFallback(b);
    }

    // Logs a bot's camera view mode and firing state changes, the server-side effects of the
    // camera and fire buttons (docs/findings/module-v1.md, Q11).
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
             m_tick, static_cast<unsigned>(id), o->getTemplate() ? o->getTemplate()->getName().c_str() : "?", what,
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

    void sendVehicleList()
    {
        if (!m_ready || !m_conn || m_tick < m_vehicleListDue)
            return;
        m_vehicleListDue = m_tick + kVehicleListPeriod;
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
            e.templateName = o->getTemplate() ? o->getTemplate()->getName() : std::string();
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
        m_vehicleBuf.clear();
        proto::encodeVehicleList(m_vehicleBuf, static_cast<uint32_t>(m_tick - 1), m_vehicleEntries);
        g_channel->sendVehicles(m_conn, m_vehicleBuf);
    }

    // ---------------------------------------------------------------- state

    void fillEntry(world::IPlayer* p, uint8_t flags, proto::StateEntry& e)
    {
        e.playerId = static_cast<uint8_t>(p->getId());
        e.team = static_cast<uint8_t>(p->getTeam());
        e.seat = proto::kSeatOnFoot;
        e.rootId = 0;
        for (int k = 0; k < 3; ++k)
            e.pos[k] = e.ypr[k] = e.vel[k] = e.angVel[k] = 0.0f;
        e.health = -1.0f;

        IObject* root = 0;
        IPlayerControlObject* seat = seatOf(p, &root);
        const bool alive = p->getIsAlive();
        if (alive)
            flags |= proto::kFlagAlive;
        if (seat)
        {
            flags |= proto::kFlagInVehicle;
            e.seat = static_cast<uint8_t>(seat->getPcoId());
        }
        e.flags = flags;
        if (!alive || !root)
            return;

        io::INetworkable* net = root->getNetworkable();
        e.rootId = net ? net->m_networkableId : 0;
        const Mat4& m = root->getAbsoluteTransformation();
        const float r2d = 57.29577951f;
        float s = m.m[9];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        e.pos[0] = m.m[12];
        e.pos[1] = m.m[13];
        e.pos[2] = m.m[14];
        e.ypr[0] = std::atan2(m.m[8], m.m[10]) * r2d;
        e.ypr[1] = -std::asin(s) * r2d;
        e.ypr[2] = std::atan2(m.m[1], m.m[5]) * r2d;
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

    void sendState()
    {
        if (!m_ready || !m_conn)
            return;
        m_entries.clear();
        for (size_t i = 0; i < m_bots.size(); ++i)
        {
            const Bot& b = m_bots[i];
            if (b.conn != m_conn || b.removeCountdown >= 0)
                continue;
            world::IPlayer* p = live(b);
            if (!p)
                continue;
            m_entries.push_back(proto::StateEntry());
            fillEntry(p, proto::kFlagOwnBot, m_entries.back());
        }
        const std::list<world::IPlayer*>& players = m_players->getPlayers();
        for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
        {
            world::IPlayer* p = *it;
            if (!p || !seatOf(p, 0) || isOwnBot(p))
                continue;
            m_entries.push_back(proto::StateEntry());
            fillEntry(p, 0, m_entries.back());
        }
        const double time = m_system ? m_system->getExactTime() : (m_tick - 1) / static_cast<double>(kTickRate);
        m_stateBuf.clear();
        // The snapshot is the end of the previous simulation tick (Q1).
        proto::encodeTickState(m_stateBuf, static_cast<uint32_t>(m_tick - 1), time, m_entries);
        g_channel->sendState(m_conn, m_stateBuf);
    }

    bool isOwnBot(world::IPlayer* p) const
    {
        for (size_t i = 0; i < m_bots.size(); ++i)
            if (m_bots[i].player == p && m_bots[i].conn == m_conn && m_bots[i].removeCountdown < 0)
                return true;
        return false;
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
    uint32_t m_conn;
    bool m_ready;
    bool m_unloadSeen;
    unsigned long long m_mask;
    unsigned long m_rejectedInputs;
    unsigned long m_lastRejectLog;
    unsigned int m_scanKey;
    bool m_scanFull;
    bool m_scanAtStart = true;
    unsigned long m_vehicleListDue;
    unsigned long m_lastRestart;
    bool m_restarted;
    unsigned long m_firingLogs = 0;
    GroupInfo m_groups[kMaxSpawnGroups];
    std::map<unsigned short, IObject*> m_vehicles;   // networkable id -> root; validated on every use
    std::vector<proto::VehicleEntry> m_vehicleEntries;
    std::vector<uint8_t> m_vehicleBuf;

    std::vector<Bot> m_bots;
    std::deque<Command> m_inbox;
    std::vector<proto::StateEntry> m_entries;
    std::vector<uint8_t> m_stateBuf;
    CostStats m_cost;
};

} // namespace

extern "C" __attribute__((visibility("default"))) bool initDll(ClassManager* classManager)
{
    if (!classManager)
        return false;
    // Never destroyed: the engine keeps the listener pointer and the loader has no unload path.
    Module* module = new Module();
    if (!module->init(classManager))
    {
        delete module;
        return false;
    }
    return true;
}
