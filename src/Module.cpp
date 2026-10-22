// libserverext: the engine side of the bot channel, docs/protocol.md v0.
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
const float kNearestVehicleRadius = 30.0f;
const unsigned int kStatsPeriodTicks = 1800;   // one stats line per minute
const unsigned int kGroupRetryTicks = 150;

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

// The vehicle seat a player occupies, or null when on foot, dead or in a non-vehicle object.
IPlayerControlObject* seatOf(world::IPlayer* p, IObject** rootOut)
{
    IObject* vehicle = p->getVehicle();
    IObject* root = rootOf(vehicle);
    if (rootOut)
        *rootOut = root;
    if (!p->getIsAlive() || !vehicle || !root || root->queryInterface(IID_ISoldier))
        return 0;
    return static_cast<IPlayerControlObject*>(vehicle->queryInterface(IID_IPlayerControlObject));
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
    bool hasPending;
    unsigned int idleTicks;
    bool watchdogTripped;
    bool wasAlive;
    int removeCountdown;       // > 0 after the sidecar disconnected
    int missingTicks;
    unsigned long lastGroupTry;
};

class Module : public IEventListener
{
public:
    Module()
        : m_em(0), m_ext(0), m_players(0), m_objects(0), m_game(0), m_server(0), m_system(0),
          m_tick(0), m_conn(0), m_ready(false), m_unloadSeen(false), m_mask(humanMappingMask()),
          m_rejectedInputs(0), m_lastRejectLog(0), m_buttonsLogged(false) {}

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
        logLine("loaded: protocol v%u, bot channel %s:%d, watchdog %d ticks; tick hook %d (event %u), unload hook %d, System %p",
             static_cast<unsigned>(proto::kVersion), m_config.listenHost.c_str(), m_config.listenPort,
             m_config.watchdogTicks, tickOk ? 1 : 0, handle, hudOk ? 1 : 0, (void*)m_system);
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
        updateBots();
        sendState();
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
        const int group = m_ext->findSpawnGroup(m.team, false);
        if (group < 0)
        {
            error(m.requestId, proto::ErrRefused, "no active on-foot spawn group for that team (is a level loaded?)");
            logLine("tick %lu: CreateBot '%s' team %d refused: findSpawnGroup -> %d", m_tick, name.c_str(), m.team, group);
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
        p->setSpawnGroup(group);

        Bot b;
        b.id = id;
        b.player = p;
        b.conn = m_conn;
        b.team = m.team;
        for (int i = 0; i < proto::AxisCount; ++i)
            b.axes[i] = b.pending[i] = 0.0f;
        b.hasPending = false;
        b.idleTicks = 0;
        b.watchdogTripped = false;
        b.wasAlive = p->getIsAlive();
        b.removeCountdown = -1;
        b.missingTicks = 0;
        b.lastGroupTry = m_tick;
        m_bots.push_back(b);

        std::vector<uint8_t> f;
        proto::encodeBotCreated(f, m.requestId, static_cast<uint8_t>(id));
        control(f);
        logLine("tick %lu: created bot %d '%s' team %d kit %d spawn group %d (ai %d connected %d)", m_tick, id,
             p->getName().c_str(), m.team, kDefaultKit, group, p->getIsAIPlayer() ? 1 : 0, p->getIsConnected() ? 1 : 0);
    }

    // The seat object with PCO id seat on vehicle root, if that seat is free and the vehicle's
    // team is 0 or team.
    IObject* freeSeat(IObject* root, int seat, int team, const char** why)
    {
        IPlayerControlObject* pco = static_cast<IPlayerControlObject*>(root->queryInterface(IID_IPlayerControlObject));
        if (!pco || root->queryInterface(IID_ISoldier))
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

        const char* why = "no vehicle with that seat free within 30 m";
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
            seat = freeSeat(root, m.seat, b->team, &why);
        }
        else
        {
            const Vec3& myPos = me->getAbsolutePosition();
            float best = kNearestVehicleRadius;
            const std::map<unsigned int, IObject*>& all = m_objects->getAllRegisteredObjects();
            for (std::map<unsigned int, IObject*>::const_iterator it = all.begin(); it != all.end(); ++it)
            {
                IObject* o = it->second;
                if (!o || o->getParent() || o == me)
                    continue;
                const float d = distance(o->getAbsolutePosition(), myPos);
                if (d >= best)
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
        if (m.buttons != 0 && !m_buttonsLogged)
        {
            logLine("tick %lu: SetInput buttons 0x%08X ignored; buttons are reserved in v0 (logged once)", m_tick, m.buttons);
            m_buttonsLogged = true;
        }
        for (int i = 0; i < proto::AxisCount; ++i)
            b->pending[i] = axes[i];   // latest wins within a tick
        b->hasPending = true;
    }

    // ---------------------------------------------------------------- per-tick bot work

    // Watchdog neutral: cyclic and pedals 0, collective held while in a vehicle. On foot every axis
    // is 0, since a held collective would keep the soldier walking.
    void neutralise(Bot& b, world::IPlayer* p)
    {
        const bool inVehicle = p && seatOf(p, 0);
        b.axes[proto::AxisPitch] = 0.0f;
        b.axes[proto::AxisRoll] = 0.0f;
        b.axes[proto::AxisPedals] = 0.0f;
        if (!inVehicle)
            b.axes[proto::AxisCollective] = 0.0f;
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
        in->inputs[io::PIYaw] = b.axes[proto::AxisPedals];
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
                logLine("tick %lu: bot %d %s", m_tick, b.id, alive ? "spawned" : "died");
                if (!alive)
                    for (int k = 0; k < proto::AxisCount; ++k)
                        b.axes[k] = 0.0f;
                b.wasAlive = alive;
            }
            if (!alive && p->getSpawnGroup() == 0 && m_tick - b.lastGroupTry >= kGroupRetryTicks)
            {
                // uPlayingSpawning clears a stale group; give the bot its team's on-foot group again.
                b.lastGroupTry = m_tick;
                const int group = m_ext->findSpawnGroup(b.team, false);
                if (group >= 0)
                    p->setSpawnGroup(group);
            }

            if (b.hasPending)
            {
                for (int k = 0; k < proto::AxisCount; ++k)
                    b.axes[k] = b.pending[k];
                b.hasPending = false;
                b.idleTicks = 0;
                if (b.watchdogTripped)
                    logLine("tick %lu: bot %d input resumed", m_tick, b.id);
                b.watchdogTripped = false;
            }
            else if (++b.idleTicks >= static_cast<unsigned int>(m_config.watchdogTicks) && !b.watchdogTripped)
            {
                neutralise(b, p);
                logLine("tick %lu: bot %d watchdog: no SetInput for %u ticks; cyclic and pedals 0, collective %.2f",
                     m_tick, b.id, b.idleTicks, b.axes[proto::AxisCollective]);
            }
            writeInput(b, p);
            ++i;
        }
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
    bool m_buttonsLogged;

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
