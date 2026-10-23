// The engine side of the admin channel (Admin.h). Everything here runs on the server's main thread.
// Engine access is typed virtual calls, inline header code and the IServerExtension methods.

#include "Admin.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "Core/ClassManager.h"
#include "Core/EventManager.h"
#include "Core/Ids.h"
#include "Core/System.h"
#include "Game/GameServer/IServerExtension.h"
#include "Game/IEventListener.h"
#include "Game/IGame.h"
#include "Game/IGameServer.h"
#include "Game/Objects/IControlPoint.h"
#include "IO/Console/Console.h"
#include "Physics/IPhysicsNode.h"
#include "Scene/Object/ObjectManager.h"
#include "Scene/Object/ObjectTypes/Base/ObjectTemplateManager.h"
#include "Scene/Object/PlayerManager.h"
#include "Scene/Object/PlayerScoreData.h"

#include "AdminIo.h"
#include "AdminLink.h"
#include "AdminProtocol.h"
#include "Config.h"
#include "EngineUtil.h"
#include "Rotation.h"

using namespace dice::hfe;
using namespace dice::hfe::world;
using namespace serverext::engine;

namespace serverext {

namespace {

using namespace admin;

typedef IServerExtension::ScriptEventArg Arg;

const unsigned int kGameStatusEventId = 1;    // ECGameLogic: ServerGameLogic::setGameStatus
const unsigned int kUnloadHudEventId = 3;     // ECHud: GameServer::unload
const int kStatusPlaying = 1, kStatusEndGame = 2, kStatusPaused = 4;
const size_t kMaxCommandsPerTick = 64;
const size_t kMaxTrackedProjectiles = 256;
const unsigned int kCachedObjectFlag = 0x20;
const uint32_t kNoNetIdBase = 0x10000;
const unsigned long kStatsPeriodTicks = 1800;
const unsigned long kCpRescanTicks = 300;
const int kMaxMaps = 256;
const size_t kMaxChatText = 1024;

// Script-event ids (Game/Python/PythonHost.cpp scriptEventMap).
enum ScriptEventId
{
    SePlayerKilled = 1, SePlayerRevived = 2, SePlayerDeath = 3, SePlayerConnect = 14, SePlayerDisconnect = 15,
    SePlayerBanned = 16, SePlayerKicked = 17, SeEnterVehicle = 21, SeExitVehicle = 22, SePlayerSpawn = 23,
    SeVehicleDestroyed = 24, SeControlPoint = 25, SeChatMessage = 39
};

// GameServer::handleStringReceivedEvent's channel names, by channel number.
const char* const kChannelNames[] = {
    "Global", "Team", "Commander", "Squad", "SquadLeader", "SquadId", "LocalTeamMedic", "LocalGroundVehicle",
    "LocalAirVehicle", "Local", "LocalTeam", "Vehicle", "ServerMessage", "ServerTeamMessage", "Player", "LocalRevive",
    "LocalRevive", "TeamAndLocalEnemies", "SquadAndLocalEnemies", "Invalid" };

std::string lower(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = static_cast<char>(out[i] - 'A' + 'a');
    return out;
}

bool contains(const std::vector<std::string>& lowered, const std::string& name)
{
    if (lowered.empty())
        return false;
    const std::string l = lower(name);
    return std::find(lowered.begin(), lowered.end(), l) != lowered.end();
}

uint8_t clampU8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

uint32_t objectIdOf(IObject* o) { return o ? o->getObjectId() : kNoObjectId; }

void copy3(float out[3], const Vec3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

// Removes the chat prefixes mm_webadmin's stripmessage removed (engine bytes; 0xA7 is the colour
// code prefix). @return True when a dead prefix was among them.
bool stripChatPrefixes(std::string& text)
{
    static const char* const kPrefixes[] = { "HUD_TEXT_CHAT_TEAM", "HUD_TEXT_CHAT_SQUAD", "HUD_TEXT_CHAT_COMMANDER" };
    static const char* const kDead[] = { "HUD_CHAT_DEADPREFIX", "*\xA7" "1DEAD\xA7" "0*", "*DEAD*", "\xA7" "1DEAD\xA7" "0" };
    bool dead = false;
    for (size_t k = 0; k < sizeof(kPrefixes) / sizeof(kPrefixes[0]); ++k)
    {
        size_t at;
        while ((at = text.find(kPrefixes[k])) != std::string::npos)
            text.erase(at, std::strlen(kPrefixes[k]));
    }
    for (size_t k = 0; k < sizeof(kDead) / sizeof(kDead[0]); ++k)
    {
        size_t at;
        while ((at = text.find(kDead[k])) != std::string::npos)
        {
            text.erase(at, std::strlen(kDead[k]));
            dead = true;
        }
    }
    return dead;
}

// Microsecond histogram for the admin channel's own per-tick cost.
class Histogram
{
public:
    Histogram() { reset(); }
    void add(double us)
    {
        const int b = us < 0 ? 0 : static_cast<int>(us / 10.0);
        ++m_buckets[b >= kBuckets ? kBuckets - 1 : b];
        ++m_count;
        if (us > m_max) m_max = us;
    }
    double pct(double q) const
    {
        if (!m_count) return 0;
        const unsigned long long target = static_cast<unsigned long long>(std::ceil(q * m_count));
        unsigned long long acc = 0;
        for (int i = 0; i < kBuckets; ++i)
        {
            acc += m_buckets[i];
            if (acc >= target) return (i + 1) * 10.0;
        }
        return kBuckets * 10.0;
    }
    double max() const { return m_max; }
    unsigned long long count() const { return m_count; }
    void reset()
    {
        for (int i = 0; i < kBuckets; ++i) m_buckets[i] = 0;
        m_count = 0;
        m_max = 0;
    }

private:
    static const int kBuckets = 2000;
    unsigned long long m_buckets[kBuckets];
    unsigned long long m_count;
    double m_max;
};

class Admin : public AdminChannel,
              public IEventListener,
              public IServerExtension::IScriptEventListener,
              public IServerExtension::IDamageObserver,
              public IServerExtension::IProjectileObserver
{
public:
    Admin(const Config& config, AdminHost* host, AdminLogFn log)
        : m_config(config), m_host(host), m_logFn(log), m_em(0), m_ext(0), m_players(0), m_objects(0), m_game(0),
          m_server(0), m_system(0), m_console(0), m_templates(0), m_link(0), m_io(0), m_session(0), m_seq(0),
          m_tick(0), m_lastStatus(-2), m_lastBeginRound(-2), m_cpsScanned(false), m_lastCpScan(0), m_damageEvents(false),
          m_projectileEvents(false), m_r4(false), m_eventsSent(0), m_eventsDropped(0), m_commands(0)
    {
        for (int i = 0; i < kCategories; ++i)
            m_bytes[i] = 0;
    }

    bool init(ClassManager* cm, std::string& error)
    {
        m_em = reinterpret_cast<EventManager*>(cm->getSingleton("EventManager"));
        m_ext = reinterpret_cast<IServerExtension*>(cm->getSingleton("OpenBF2ServerExtension"));
        m_players = reinterpret_cast<PlayerManager*>(cm->getSingleton("PlayerManager"));
        m_objects = reinterpret_cast<ObjectManager*>(cm->getSingleton("ObjectManager"));
        m_game = reinterpret_cast<IGame*>(cm->getSingleton("Game"));
        m_system = reinterpret_cast<System*>(cm->getSingleton("System"));
        m_console = reinterpret_cast<io::Console*>(cm->getSingleton("MainConsole"));
        m_templates = reinterpret_cast<ObjectTemplateManager*>(cm->getSingleton("ObjectTemplateManager"));
        m_server = m_game ? static_cast<IGameServer*>(m_game->queryInterface(IID_IGameServer)) : 0;
        if (!m_em || !m_ext || !m_players || !m_objects || !m_game || !m_server || !m_console || !m_templates)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "a required engine singleton is missing (MainConsole %p, ObjectTemplateManager %p)",
                          (void*)m_console, (void*)m_templates);
            error = buf;
            return false;
        }
        if (!m_ext->setScriptEventListener(this))
        {
            error = "another module already holds the script-event listener (R4)";
            return false;
        }
        m_r4 = true;

        m_link = new AdminLink(static_cast<size_t>(m_config.adminQueueBytes));   // lives for the process
        AdminIoConfig io;
        io.host = m_config.adminHost;
        io.port = m_config.adminPort;
        io.serverId = m_config.adminServerId;
        io.key = m_config.adminKey;
        io.tls = m_config.adminTls;
        io.pin = m_config.adminCertSha256;
        io.build = "libserverext admin 1";
        m_io = new AdminIo(io, m_link, &Admin::logFromIo);
        s_logFn = m_logFn;
        if (!m_io->start(error))
        {
            m_ext->setScriptEventListener(0);
            return false;
        }
        const bool statusOk = m_em->registerEventHandler(ECGameLogic, this, 0);
        const bool hudOk = m_em->registerEventHandler(ECHud, this, 0);
        const bool damageOk = m_ext->addDamageObserver(this);
        const bool projectileOk = m_ext->addProjectileObserver(this);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "admin channel: protocol v%u, dial %s:%d, server id '%s', TLS %s, identity %d, console %d, objects %d, "
                      "queue %d bytes; R4 1, status hook %d, unload hook %d, damage %d, projectile %d",
                      static_cast<unsigned>(kVersion), m_config.adminHost.c_str(), m_config.adminPort,
                      m_config.adminServerId.c_str(), m_config.adminTls ? "1.3 (pinned)" : "off (loopback test)",
                      m_config.adminIdentity() ? 1 : 0, m_config.adminAllowConsole ? 1 : 0,
                      m_config.adminAllowObjects ? 1 : 0, m_config.adminQueueBytes, statusOk ? 1 : 0, hudOk ? 1 : 0,
                      damageOk ? 1 : 0, projectileOk ? 1 : 0);
        m_describe = buf;
        return true;
    }

    virtual std::string describe() const { return m_describe; }

    // ---------------------------------------------------------------- per tick

    virtual void tick(unsigned long tick)
    {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        m_tick = tick;
        processCommands();
        if (m_session)
        {
            pollGameState();
            if (!m_cpsScanned || (m_cps.empty() && m_tick - m_lastCpScan >= kCpRescanTicks))
                scanControlPoints();
            flushControlPointEvents();
            pollPlayers();
            updateProjectiles();
            sendStreams();
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        m_cost.add(us);
        if (m_tick % kStatsPeriodTicks == 0)
            reportStats();
    }

    virtual void onBot(uint8_t kind, int playerId, int team, uint32_t connection)
    {
        if (!m_session || playerId < 0 || playerId > 255)
            return;
        std::vector<uint8_t> f;
        encodeBot(f, ++m_seq, tickNumber(), static_cast<uint8_t>(playerId), kind, clampU8(team), connection);
        event(f);
    }

    // ---------------------------------------------------------------- engine listeners

    virtual void handleEvent(EventCategory category, unsigned int id, EventNode*, void* data)
    {
        if (category == ECHud && id == kUnloadHudEventId)
        {
            m_cps.clear();
            m_cpEvents.clear();
            m_cpsScanned = false;
            m_projectiles.clear();
            logf("tick %lu: admin: level unload; control points and projectiles forgotten", m_tick);
            return;
        }
        if (category == ECGameLogic && id == kGameStatusEventId && data)
            onGameStatus(*static_cast<const int*>(data));
    }

    // R4. Copies what the events need; no engine object is created, destroyed or damaged here.
    virtual void onScriptEvent(int eventId, const Arg* args, int argc)
    {
        if (eventId == SePlayerKicked || eventId == SePlayerBanned)
        {
            // Remembered even without a link, for the disconnect reason.
            world::IPlayer* p = argPlayer(args, argc, 0);
            if (p)
            {
                Track& t = m_tracks[p->getId()];
                if (eventId == SePlayerKicked)
                    t.kicked = true;
                else
                    t.banned = true;
            }
        }
        if (!m_session)
            return;
        switch (eventId)
        {
        case SePlayerKilled: onKill(args, argc); break;
        case SePlayerRevived: onRevive(args, argc); break;
        case SePlayerDeath: onDeath(args, argc); break;
        case SePlayerConnect: onConnect(args, argc); break;
        case SePlayerDisconnect: onDisconnect(args, argc); break;
        case SePlayerBanned: onBanned(args, argc); break;
        case SePlayerKicked: onKicked(args, argc); break;
        case SeEnterVehicle: onSeat(EvEnterVehicle, args, argc); break;
        case SeExitVehicle: onSeat(EvExitVehicle, args, argc); break;
        case SePlayerSpawn: onSpawn(args, argc); break;
        case SeVehicleDestroyed: onVehicleDestroyed(args, argc); break;
        case SeControlPoint: onControlPoint(args, argc); break;
        case SeChatMessage: onChat(args, argc); break;
        default: break;
        }
    }

    // R5.
    virtual void onDamage(const IServerExtension::DamageInfo& d)
    {
        if (!m_session)
            return;
        if (d.source)
            for (size_t i = 0; i < m_projectiles.size(); ++i)
                if (m_projectiles[i].object == d.source && !m_projectiles[i].damaged)
                {
                    m_projectiles[i].damaged = true;
                    m_projectiles[i].damagedNetId = netIdOf(rootOf(d.victim));
                }
        if (!m_damageEvents)
            return;
        IObject* root = rootOf(d.victim);
        DamageEvent e;
        world::IPlayer* vp = playerOfRoot(root);
        if (!vp && d.killedCount > 0)
            vp = d.killed[0];
        e.victimPlayer = vp ? static_cast<int16_t>(vp->getId()) : -1;
        e.victimRootNetId = netIdOf(root);
        e.victimRootObjectId = objectIdOf(root);
        e.victimTemplate = templateName(d.victim);
        e.attacker = d.attacker ? static_cast<int16_t>(d.attacker->getId()) : -1;
        e.weapon = templateName(d.weapon);
        e.projectile = templateName(d.source);
        e.amount = d.amount;
        e.before = d.hitPointsBefore;
        e.after = d.hitPointsAfter;
        e.max = d.maxHitPoints;
        for (int k = 0; k < 3; ++k)
            e.pos[k] = d.position[k];
        e.flags = (!(d.hitPointsAfter > 0.0f) ? 1 : 0) | (d.killedCount > 0 ? 2 : 0);
        std::vector<uint8_t> f;
        encodeDamage(f, ++m_seq, tickNumber(), e);
        event(f);
    }

    virtual void onProjectileLaunched(IObject* projectile, world::IPlayer*)
    {
        if (!m_session || !projectile || m_projectiles.size() >= kMaxTrackedProjectiles)
            return;
        const std::string name = templateName(projectile);
        const bool streamed = m_streams[StreamProjectiles].on && contains(m_streams[StreamProjectiles].filters, name);
        const bool evented = m_projectileEvents && contains(m_projectileEventFilters, name);
        if (!streamed && !evented)
            return;
        Projectile t;
        t.object = projectile;
        t.objectId = projectile->getObjectId();
        const uint32_t net = netIdOf(projectile);
        t.wireId = net ? net : kNoNetIdBase + t.objectId;
        t.templateName = name;
        t.damaged = false;
        t.damagedNetId = 0;
        t.timeToLive = 0.0f;
        t.owner = -1;
        for (int k = 0; k < 3; ++k)
            t.lastPos[k] = 0.0f;
        ProjectileState s;
        if (!fillProjectile(t, projectile, s))
            return;
        m_projectiles.push_back(t);
        if (evented)
        {
            std::vector<uint8_t> f;
            encodeProjectileFired(f, ++m_seq, tickNumber(), s);
            event(f);
        }
    }

private:
    struct Track
    {
        world::IPlayer* player;        // borrowed; compared with getPlayerFromId, never dereferenced first
        int team;
        PlayerScore score;
        bool kicked, banned;
        Track() : player(0), team(-1), kicked(false), banned(false) {}
    };

    struct ControlPoint
    {
        unsigned int objectId;
        IObject* object;        // borrowed; validated through getRegisteredObject
    };

    struct Projectile
    {
        IObject* object;        // borrowed; used only after getRegisteredObject(objectId) returns it
        unsigned int objectId;
        uint32_t wireId;
        std::string templateName;
        bool damaged;           // R5 saw it as the source of damage
        uint32_t damagedNetId;  // the first damaged root's networkable id, 0 if none
        float timeToLive;
        float lastPos[3];
        int16_t owner;
    };

    struct StreamSub
    {
        bool on;
        unsigned int period;
        unsigned long due;
        std::vector<std::string> filters;   // lowered template names
        StreamSub() : on(false), period(1), due(0) {}
    };

    enum Category { CatEvents = 0, CatReplies, CatPlayers, CatVehicles, CatProjectiles, CatGame, kCategories };

    // ---------------------------------------------------------------- logging and output

    static AdminLogFn s_logFn;
    static void logFromIo(const std::string& line) { if (s_logFn) s_logFn(line.c_str()); }

    void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char buf[768];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (m_logFn)
            m_logFn(buf);
    }

    // The tick number of the state the main thread last finished: the module's tick N handler
    // reports the end of tick N-1, as the bot channel's TickState does.
    uint32_t tickNumber() const { return static_cast<uint32_t>(m_tick ? m_tick - 1 : 0); }
    double now() const { return m_system ? m_system->getExactTime() : m_tick / 30.0; }

    void event(std::vector<uint8_t>& f)
    {
        const size_t n = f.size();
        if (m_link->sendEvent(m_session, f))
        {
            ++m_eventsSent;
            m_bytes[CatEvents] += n;
        }
        else
            ++m_eventsDropped;
    }

    void reply(std::vector<uint8_t>& f)
    {
        m_bytes[CatReplies] += f.size();
        m_link->sendReply(m_session, f);
    }

    void ok(uint32_t requestId)
    {
        std::vector<uint8_t> f;
        encodeOk(f, requestId);
        reply(f);
    }

    void error(uint32_t requestId, uint16_t code, const std::string& message)
    {
        std::vector<uint8_t> f;
        encodeError(f, requestId, code, message);
        reply(f);
    }

    void reportStats()
    {
        const double seconds = kStatsPeriodTicks / 30.0;
        logf("tick %lu: admin: session %u, events %llu sent %llu dropped, commands %llu; B/s events %.0f replies %.0f players %.0f vehicles %.0f projectiles %.0f game %.0f; admin cost (us) p50 %.0f p99 %.0f max %.0f",
             m_tick, m_session, static_cast<unsigned long long>(m_eventsSent), static_cast<unsigned long long>(m_eventsDropped),
             static_cast<unsigned long long>(m_commands), m_bytes[CatEvents] / seconds, m_bytes[CatReplies] / seconds,
             m_bytes[CatPlayers] / seconds, m_bytes[CatVehicles] / seconds, m_bytes[CatProjectiles] / seconds,
             m_bytes[CatGame] / seconds, m_cost.pct(0.5), m_cost.pct(0.99), m_cost.max());
        for (int i = 0; i < kCategories; ++i)
            m_bytes[i] = 0;
        m_cost.reset();
    }

    // ---------------------------------------------------------------- argument helpers

    static const Arg* arg(const Arg* args, int argc, int i) { return i < argc ? &args[i] : 0; }

    static world::IPlayer* argPlayer(const Arg* args, int argc, int i)
    {
        const Arg* a = arg(args, argc, i);
        return a && a->kind == Arg::Player ? a->player : 0;
    }

    static IObject* argObject(const Arg* args, int argc, int i)
    {
        const Arg* a = arg(args, argc, i);
        return a && a->kind == Arg::Object ? a->object : 0;
    }

    static int argInt(const Arg* args, int argc, int i, int fallback)
    {
        const Arg* a = arg(args, argc, i);
        return a && a->kind == Arg::Int ? a->i : fallback;
    }

    static std::string argString(const Arg* args, int argc, int i)
    {
        const Arg* a = arg(args, argc, i);
        return a && a->kind == Arg::String && a->s ? std::string(a->s, static_cast<size_t>(a->sLength)) : std::string();
    }

    // ---------------------------------------------------------------- engine reads

    // The player whose soldier is root, or the occupant of a vehicle root's first occupied seat.
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

    // The players seated in a vehicle root (all its seats).
    static int countOccupants(IObject* root)
    {
        IPlayerControlObject* pco = vehiclePco(root);
        if (!pco)
            return 0;
        int n = pco->getOccupingPlayer() ? 1 : 0;
        std::map<int, IPlayerControlObject*>* seats = pco->getPcos();
        if (seats)
            for (std::map<int, IPlayerControlObject*>::iterator s = seats->begin(); s != seats->end(); ++s)
                if (s->second && s->second != pco && s->second->getOccupingPlayer())
                    ++n;
        return n;
    }

    // True for a soldier or a vehicle with someone in it: zero hit points must not reach those
    // through setHitPoints (see onKillPlayer).
    bool occupied(IObject* root)
    {
        return root && (root->queryInterface(IID_ISoldier) || countOccupants(root) > 0);
    }

    // A player's root object: the vehicle when seated, else the soldier; null when dead.
    IObject* rootOfPlayer(world::IPlayer* p)
    {
        if (!p || !p->getIsAlive())
            return 0;
        IObject* root = rootOf(p->getVehicle());
        return root && !root->queryInterface(IID_ICameraObject) ? root : 0;
    }

    static float healthFraction(IObject* o)
    {
        return o ? healthOf(o, 0) : -1.0f;
    }

    static void velocityOf(IObject* o, float out[3])
    {
        out[0] = out[1] = out[2] = 0.0f;
        IPhysicsNode* node = o ? static_cast<IPhysicsNode*>(o->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)) : 0;
        if (node)
            copy3(out, node->getPositionalSpeed());
    }

    static void placeOf(IObject* o, float pos[3], float rot[3])
    {
        const Mat4& m = o->getAbsoluteTransformation();
        pos[0] = m.m[12]; pos[1] = m.m[13]; pos[2] = m.m[14];
        attitudeFromMatrix(m.m, rot);
    }

    void fillScore(world::IPlayer* p, PlayerScore& s)
    {
        PlayerScoreData* d = p->getScoreData();
        if (!d)
            return;
        s.score = d->m_score;
        s.teamScore = d->m_field04;
        s.kills = d->m_field14;
        s.deaths = d->m_field10;
        s.teamKills = d->m_field18;
        s.rank = static_cast<int16_t>(d->m_rank < -1 ? -1 : d->m_rank > 32767 ? 32767 : d->m_rank);
    }

    void fillPlayerState(world::IPlayer* p, PlayerState& s)
    {
        s = PlayerState();
        s.team = clampU8(p->getTeam());
        s.squad = clampU8(p->getSquadId());
        const bool alive = p->getIsAlive();
        s.life = alive ? (p->getIsManDown() ? kLifeManDown : kLifeAlive) : kLifeDead;
        IObject* root = 0;
        IPlayerControlObject* seat = seatOf(p, &root);
        s.flags = (seat ? kPlayerInVehicle : 0) | (p->getIsCommander() ? kPlayerCommander : 0) |
                  (p->getIsSquadLeader() ? kPlayerSquadLeader : 0);
        const unsigned int ping = p->getPing();
        s.ping = static_cast<uint16_t>(ping > 0xFFFF ? 0xFFFF : ping);
        if (seat)
            s.seat = clampU8(seat->getPcoId());
        root = rootOfPlayer(p);
        if (!root)
            return;
        s.rootNetId = netIdOf(root);
        s.rootObjectId = objectIdOf(root);
        placeOf(root, s.pos, s.rot);
        velocityOf(root, s.vel);
        s.health = alive ? healthFraction(p->getDefaultVehicle()) : -1.0f;
    }

    void fillPlayerInfo(world::IPlayer* p, PlayerInfo& info)
    {
        info = PlayerInfo();
        info.id = static_cast<uint8_t>(p->getId());
        info.name = cp1252ToUtf8(p->getName());
        uint32_t conn = 0;
        info.flags = (p->getIsAIPlayer() ? kInfoAi : 0) | (m_host->botChannelBot(p->getId(), &conn) ? kInfoBotChannel : 0) |
                     (p->getIsConnected() ? kInfoConnected : 0);
        info.profileId = p->getProfileId();
        if (m_config.adminIdentity())
        {
            info.flags |= kInfoIdentity;
            if (p->getIsRemote())
            {
                std::string ip;
                if (m_server->getPlayerConnectionIp(p, ip))
                    info.ip = ip;
                info.hash = m_ext->getCdKeyHash(p->getId());
            }
        }
        fillScore(p, info.score);
        fillPlayerState(p, info.state);
    }

    void fillVehicle(IObject* o, VehicleState& v)
    {
        v = VehicleState();
        IPlayerControlObject* pco = vehiclePco(o);
        v.netId = netIdOf(o);
        v.objectId = objectIdOf(o);
        v.templateName = cp1252ToUtf8(templateName(o));
        v.team = pco ? clampU8(pco->getTeam()) : 0;
        bool destroyed = false;
        v.health = healthOf(o, &destroyed);
        v.flags = destroyed ? 1 : 0;
        placeOf(o, v.pos, v.rot);
        velocityOf(o, v.vel);
        std::map<int, IPlayerControlObject*>* seats = pco ? pco->getPcos() : 0;
        if (seats)
        {
            v.seats = static_cast<uint8_t>(seats->size() > 255 ? 255 : seats->size());
            for (std::map<int, IPlayerControlObject*>::iterator s = seats->begin(); s != seats->end(); ++s)
                if (s->first >= 0 && s->first < 32 && s->second && s->second->isOccupied())
                    v.occupied |= 1u << s->first;
        }
    }

    bool fillControlPoint(IObject* o, ControlPointState& c)
    {
        IControlPoint* cp = o ? static_cast<IControlPoint*>(o->queryInterface(IID_IControlPoint)) : 0;
        if (!cp)
            return false;
        c = ControlPointState();
        c.objectId = objectIdOf(o);
        c.netId = netIdOf(o);
        c.templateName = cp1252ToUtf8(templateName(o));
        c.team = clampU8(cp->getTeam());
        c.flagTeam = clampU8(cp->getFlag());
        c.takeOver = cp->getTakeOverCounter();
        c.capturing = clampU8(cp->getCapturingState());
        copy3(c.pos, o->getAbsolutePosition());
        return true;
    }

    // Fills s from the engine; also refreshes t's last position, time to live and owner.
    bool fillProjectile(Projectile& t, IObject* o, ProjectileState& s)
    {
        IServerExtension::ProjectileInfo info;
        if (!m_ext->getProjectileInfo(o, info))
            return false;
        s = ProjectileState();
        s.id = t.wireId;
        s.templateName = cp1252ToUtf8(t.templateName);
        s.owner = info.ownerPlayerId >= 0 && info.ownerPlayerId <= 255 ? static_cast<int16_t>(info.ownerPlayerId) : -1;
        s.team = clampU8(info.team);
        s.flags = info.guided ? 1 : 0;
        s.targetNetId = netIdOf(rootOf(info.target));
        placeOf(o, s.pos, s.rot);
        velocityOf(o, s.vel);
        s.timeToLive = info.timeToLive;
        t.timeToLive = info.timeToLive;
        t.owner = s.owner;
        for (int k = 0; k < 3; ++k)
            t.lastPos[k] = s.pos[k];
        return info.active;
    }

    void fillGameState(GameState& g)
    {
        g = GameState();
        const int status = m_ext->getGameStatus();
        const int brs = m_ext->getBeginRoundState();
        g.status = clampU8(status < 0 ? 0 : status);
        g.beginRound = static_cast<uint8_t>(brs < 0 || brs > 254 ? 255 : brs);
        g.timeToNext = m_ext->getTimeToNextStateChange();
        IServerExtension::RoundInfo round;
        if (m_ext->getRoundInfo(round))
        {
            g.tickets[0] = round.tickets[0];
            g.tickets[1] = round.tickets[1];
            g.roundTime = round.roundTime;
            g.winner = clampU8(round.winner);
            g.victoryType = clampU8(round.victoryType);
        }
        IServerExtension::ServerInfo server;
        if (m_ext->getServerInfo(server))
            g.timeLimit = static_cast<float>(server.timeLimit);
    }

    // ---------------------------------------------------------------- session

    void processCommands()
    {
        m_link->drainCommands(m_inbox);
        for (std::deque<Command>::iterator it = m_inbox.begin(); it != m_inbox.end(); ++it)
        {
            if (it->type == Command::LinkUp || it->type == Command::LinkDown)
            {
                // Link changes overtake queued commands: the old link's commands are void anyway.
                if (it->type == Command::LinkUp)
                    onLinkUp(it->session);
                else if (it->session == m_session)
                    onLinkDown();
                continue;
            }
            m_pending.push_back(Command());
            std::swap(m_pending.back(), *it);
        }
        m_inbox.clear();
        size_t done = 0;
        while (!m_pending.empty() && done < kMaxCommandsPerTick)
        {
            Command c;
            std::swap(c, m_pending.front());
            m_pending.pop_front();
            if (c.session != m_session || !m_session)
                continue;
            ++done;
            ++m_commands;
            onCommand(c);
        }
    }

    void onLinkUp(uint32_t session)
    {
        m_session = session;
        m_seq = 0;
        m_pending.clear();
        for (int i = 0; i < 5; ++i)
            m_streams[i] = StreamSub();
        m_damageEvents = m_projectileEvents = false;
        m_projectileEventFilters.clear();
        m_projectiles.clear();
        m_lastStatus = m_ext->getGameStatus();
        m_lastBeginRound = m_ext->getBeginRoundState();
        // Rebuild the per-player baselines; kick and ban marks survive.
        std::map<int, Track> old;
        old.swap(m_tracks);
        const std::list<world::IPlayer*>& players = m_players->getPlayers();
        for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
        {
            world::IPlayer* p = *it;
            if (!p)
                continue;
            Track& t = m_tracks[p->getId()];
            std::map<int, Track>::iterator o = old.find(p->getId());
            if (o != old.end() && o->second.player == p)
                t = o->second;
            baseline(p, t);
        }
        if (!m_cpsScanned)
            scanControlPoints();

        WelcomeInfo w;
        w.session = session;
        w.tick = static_cast<uint32_t>(m_tick);
        w.tickRate = 30.0f;
        IServerExtension::ServerInfo server;
        if (m_ext->getServerInfo(server))
        {
            w.serverName = cp1252ToUtf8(server.serverName);
            w.gamePort = static_cast<uint16_t>(server.gamePort);
            w.gameSpyPort = static_cast<uint16_t>(server.gameSpyPort);
            w.maxPlayers = clampU8(server.maxPlayers);
        }
        w.capabilities = (m_config.adminAllowConsole ? kCapConsole : 0) | (m_config.adminAllowObjects ? kCapObjects : 0) |
                         (m_config.adminIdentity() ? kCapIdentity : 0);
        w.map = cp1252ToUtf8(m_game->getCurrentMapName());
        w.mode = cp1252ToUtf8(m_game->getCurrentMapGameMode());
        const int size = m_game->getCurrentMapSize();
        w.mapSize = static_cast<uint16_t>(size < 0 ? 0 : size > 0xFFFF ? 0xFFFF : size);
        IServerExtension::MapListEntry maps[kMaxMaps];
        int current = -1;
        const int count = std::min(m_ext->getMapList(maps, kMaxMaps, current), kMaxMaps);
        w.mapIndex = current >= 0 && current < 0xFFFF ? static_cast<uint16_t>(current) : kNoIndex;
        for (int i = 0; i < count; ++i)
        {
            MapEntry e;
            e.name = cp1252ToUtf8(maps[i].name);
            e.mode = cp1252ToUtf8(maps[i].gameMode);
            e.size = static_cast<uint16_t>(maps[i].size < 0 ? 0 : maps[i].size);
            w.maps.push_back(e);
        }
        w.team1 = cp1252ToUtf8(m_ext->getTeamName(1));
        w.team2 = cp1252ToUtf8(m_ext->getTeamName(2));
        fillGameState(w.game);
        std::vector<uint8_t> f;
        encodeWelcome(f, w);
        const size_t welcomeBytes = f.size();
        reply(f);
        const size_t snapshotBytes = sendSnapshot(0);
        logf("tick %lu: admin: link session %u authenticated; Welcome %u B (map '%s' %s, %d map-list entries), Snapshot %u B",
             m_tick, session, static_cast<unsigned>(welcomeBytes), w.map.c_str(), w.mode.c_str(), count,
             static_cast<unsigned>(snapshotBytes));
    }

    void onLinkDown()
    {
        logf("tick %lu: admin: link session %u closed after %u events", m_tick, m_session, m_seq);
        m_session = 0;
        m_pending.clear();
        m_projectiles.clear();
    }

    void baseline(world::IPlayer* p, Track& t)
    {
        t.player = p;
        t.team = p->getTeam();
        fillScore(p, t.score);
    }

    size_t sendSnapshot(uint32_t requestId)
    {
        GameState g;
        fillGameState(g);
        std::vector<PlayerInfo> players;
        const std::list<world::IPlayer*>& list = m_players->getPlayers();
        for (std::list<world::IPlayer*>::const_iterator it = list.begin(); it != list.end(); ++it)
        {
            if (!*it)
                continue;
            players.push_back(PlayerInfo());
            fillPlayerInfo(*it, players.back());
        }
        std::vector<VehicleState> vehicles;
        m_vehicleRoots.clear();
        m_host->vehicleRoots(m_vehicleRoots);
        for (size_t i = 0; i < m_vehicleRoots.size(); ++i)
        {
            vehicles.push_back(VehicleState());
            fillVehicle(m_vehicleRoots[i], vehicles.back());
        }
        std::vector<ControlPointState> cps;
        for (size_t i = 0; i < m_cps.size(); ++i)
        {
            IObject* o = liveControlPoint(m_cps[i]);
            ControlPointState c;
            if (o && fillControlPoint(o, c))
                cps.push_back(c);
        }
        std::vector<ProjectileState> projectiles;
        for (size_t i = 0; i < m_projectiles.size(); ++i)
        {
            IObject* o = liveProjectile(m_projectiles[i]);
            ProjectileState s;
            if (o && fillProjectile(m_projectiles[i], o, s))
                projectiles.push_back(s);
        }
        std::vector<uint8_t> f;
        encodeSnapshot(f, requestId, m_seq, tickNumber(), now(), g, players, vehicles, cps, projectiles);
        const size_t n = f.size();
        reply(f);
        return n;
    }

    // ---------------------------------------------------------------- polling

    void onGameStatus(int status)
    {
        if (!m_session)
        {
            m_lastStatus = status;
            return;
        }
        if (status == m_lastStatus)
            return;
        const int previous = m_lastStatus;
        m_lastStatus = status;
        m_lastBeginRound = m_ext->getBeginRoundState();
        GameState g;
        fillGameState(g);
        g.status = clampU8(status);
        std::vector<uint8_t> f;
        encodeGameStateEvent(f, ++m_seq, tickNumber(), g, clampU8(previous < 0 ? 0 : previous));
        event(f);
        if (status == kStatusPlaying && previous != kStatusPaused)
        {
            f.clear();
            encodeRoundStart(f, ++m_seq, tickNumber(), cp1252ToUtf8(m_game->getCurrentMapName()),
                             cp1252ToUtf8(m_game->getCurrentMapGameMode()), g.tickets);
            event(f);
        }
        else if (status == kStatusEndGame)
        {
            f.clear();
            encodeRoundEnd(f, ++m_seq, tickNumber(), g.tickets, g.winner, g.victoryType);
            event(f);
        }
        logf("tick %lu: admin: game status %d -> %d", m_tick, previous, status);
    }

    void pollGameState()
    {
        const int status = m_ext->getGameStatus();
        if (status != m_lastStatus)
            onGameStatus(status);   // a status change the ECGameLogic listener did not see
        const int brs = m_ext->getBeginRoundState();
        if (brs == m_lastBeginRound)
            return;
        m_lastBeginRound = brs;
        GameState g;
        fillGameState(g);
        std::vector<uint8_t> f;
        encodeGameStateEvent(f, ++m_seq, tickNumber(), g, clampU8(status < 0 ? 0 : status));
        event(f);
    }

    void pollPlayers()
    {
        const std::list<world::IPlayer*>& players = m_players->getPlayers();
        m_seen.clear();
        for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
        {
            world::IPlayer* p = *it;
            if (!p)
                continue;
            const int id = p->getId();
            m_seen.push_back(id);
            Track& t = m_tracks[id];
            if (t.player != p)
            {
                // A player the module has no baseline for (connected before the link, or a missed
                // connect): adopt it silently; the snapshot or PlayerConnect already reported it.
                const bool kicked = t.kicked, banned = t.banned;
                t = Track();
                t.kicked = kicked;
                t.banned = banned;
                baseline(p, t);
                continue;
            }
            const int team = p->getTeam();
            if (team != t.team)
            {
                uint32_t conn = 0;
                const bool bot = m_host->botChannelBot(id, &conn);
                std::vector<uint8_t> f;
                encodeTeamChange(f, ++m_seq, tickNumber(), static_cast<uint8_t>(id), clampU8(t.team), clampU8(team),
                                 bot ? 1 : 0);
                event(f);
                t.team = team;
            }
            PlayerScore s;
            fillScore(p, s);
            if (s != t.score)
            {
                std::vector<uint8_t> f;
                encodeScoreChange(f, ++m_seq, tickNumber(), static_cast<uint8_t>(id), s);
                event(f);
                t.score = s;
            }
        }
        for (std::map<int, Track>::iterator it = m_tracks.begin(); it != m_tracks.end();)
        {
            if (std::find(m_seen.begin(), m_seen.end(), it->first) == m_seen.end())
                m_tracks.erase(it++);
            else
                ++it;
        }
    }

    // ---------------------------------------------------------------- control points

    IObject* liveControlPoint(const ControlPoint& c)
    {
        IObject* o = m_objects->getRegisteredObject(static_cast<int>(c.objectId));
        return o && o == c.object ? o : 0;
    }

    void scanControlPoints()
    {
        m_cps.clear();
        const std::map<unsigned int, IObject*>& all = m_objects->getAllRegisteredObjects();
        for (std::map<unsigned int, IObject*>::const_iterator it = all.begin(); it != all.end(); ++it)
        {
            IObject* o = it->second;
            if (o && o->queryInterface(IID_IControlPoint))
            {
                ControlPoint c;
                c.objectId = o->getObjectId();
                c.object = o;
                m_cps.push_back(c);
            }
        }
        m_cpsScanned = true;
        m_lastCpScan = m_tick;
        if (!m_cps.empty())
            logf("tick %lu: admin: %u control points in %u registered objects", m_tick, static_cast<unsigned>(m_cps.size()),
                 static_cast<unsigned>(all.size()));
    }

    void flushControlPointEvents()
    {
        for (size_t i = 0; i < m_cpEvents.size(); ++i)
        {
            IObject* o = m_objects->getRegisteredObject(static_cast<int>(m_cpEvents[i].objectId));
            ControlPointState c;
            if (!o || o != m_cpEvents[i].object || !fillControlPoint(o, c))
                continue;
            std::vector<uint8_t> f;
            encodeControlPoint(f, ++m_seq, tickNumber(), m_cpEvents[i].kind, c);
            event(f);
            logf("tick %lu: admin: control point '%s' %s, team %u", m_tick, c.templateName.c_str(),
                 m_cpEvents[i].kind ? "captured" : "neutralised", c.team);
        }
        m_cpEvents.clear();
    }

    // ---------------------------------------------------------------- projectiles

    IObject* liveProjectile(const Projectile& t)
    {
        IObject* o = m_objects->getRegisteredObject(static_cast<int>(t.objectId));
        return o && o == t.object && !(o->getFlags() & kCachedObjectFlag) ? o : 0;
    }

    void updateProjectiles()
    {
        m_projectileStates.clear();
        for (size_t i = 0; i < m_projectiles.size();)
        {
            Projectile& t = m_projectiles[i];
            IObject* o = liveProjectile(t);
            ProjectileState s;
            if (o && fillProjectile(t, o, s))
            {
                if (m_streams[StreamProjectiles].on && contains(m_streams[StreamProjectiles].filters, t.templateName))
                    m_projectileStates.push_back(s);
                ++i;
                continue;
            }
            if (m_projectileEvents && contains(m_projectileEventFilters, t.templateName))
            {
                ProjectileHitEvent h;
                h.id = t.wireId;
                h.templateName = cp1252ToUtf8(t.templateName);
                h.owner = t.owner;
                h.flags = (t.timeToLive <= 1.0f / 30.0f ? 1 : 0) | (t.damaged ? 2 : 0);
                h.firstVictimNetId = t.damagedNetId;
                for (int k = 0; k < 3; ++k)
                    h.pos[k] = t.lastPos[k];
                std::vector<uint8_t> f;
                encodeProjectileHit(f, ++m_seq, tickNumber(), h);
                event(f);
            }
            m_projectiles.erase(m_projectiles.begin() + i);
        }
    }

    // ---------------------------------------------------------------- streams

    void sendStreams()
    {
        const uint32_t t = tickNumber();
        const double time = now();
        if (due(StreamPlayers))
        {
            m_ids.clear();
            m_states.clear();
            const std::list<world::IPlayer*>& players = m_players->getPlayers();
            for (std::list<world::IPlayer*>::const_iterator it = players.begin(); it != players.end(); ++it)
            {
                if (!*it)
                    continue;
                m_ids.push_back(static_cast<uint8_t>((*it)->getId()));
                m_states.push_back(PlayerState());
                fillPlayerState(*it, m_states.back());
            }
            m_frame.clear();
            encodePlayerStream(m_frame, t, time, m_ids, m_states);
            latest(AdminLink::SlotPlayers, CatPlayers);
        }
        if (due(StreamVehicles))
        {
            m_vehicleRoots.clear();
            m_host->vehicleRoots(m_vehicleRoots);
            m_vehicles.resize(m_vehicleRoots.size());
            for (size_t i = 0; i < m_vehicleRoots.size(); ++i)
                fillVehicle(m_vehicleRoots[i], m_vehicles[i]);
            m_frame.clear();
            encodeVehicleStream(m_frame, t, time, m_vehicles);
            latest(AdminLink::SlotVehicles, CatVehicles);
        }
        if (due(StreamProjectiles))
        {
            m_frame.clear();
            encodeProjectileStream(m_frame, t, time, m_projectileStates);
            latest(AdminLink::SlotProjectiles, CatProjectiles);
        }
        if (due(StreamGame))
        {
            GameState g;
            fillGameState(g);
            m_frame.clear();
            encodeGameStream(m_frame, t, time, g);
            latest(AdminLink::SlotGame, CatGame);
        }
    }

    bool due(int stream)
    {
        StreamSub& s = m_streams[stream];
        if (!s.on || m_tick < s.due)
            return false;
        s.due = m_tick + s.period;
        return true;
    }

    void latest(AdminLink::Slot slot, Category category)
    {
        m_bytes[category] += m_frame.size();
        m_link->sendLatest(m_session, slot, m_frame);
    }

    // ---------------------------------------------------------------- R4 events

    void onConnect(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        if (!p)
            return;
        Track& t = m_tracks[p->getId()];
        t = Track();
        baseline(p, t);
        PlayerInfo info;
        fillPlayerInfo(p, info);
        std::vector<uint8_t> f;
        encodePlayerConnect(f, ++m_seq, tickNumber(), info);
        event(f);
        logf("tick %lu: admin: player %d '%s' connected (ai %d, remote %d, ip '%s', hash %s)", m_tick, p->getId(),
             info.name.c_str(), p->getIsAIPlayer() ? 1 : 0, p->getIsRemote() ? 1 : 0, info.ip.c_str(),
             info.hash.empty() ? "none" : "present");
    }

    void onDisconnect(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        if (!p)
            return;
        const int id = p->getId();
        const int engineReason = m_ext->getDisconnectReason(id);
        std::map<int, Track>::iterator it = m_tracks.find(id);
        const bool kicked = it != m_tracks.end() && it->second.kicked;
        const bool banned = it != m_tracks.end() && it->second.banned;
        uint8_t reason = DiscUnknown;
        if (banned)
            reason = DiscBanned;
        else if (kicked || engineReason == 21)
            reason = DiscKicked;
        else if (engineReason == 10)
            reason = DiscTimedOut;
        else if (engineReason == 13)
            reason = DiscLeft;   // [INFERRED] a client that quits is dropped with result 13
        else if (engineReason >= 0)
            reason = DiscOther;
        std::vector<uint8_t> f;
        encodePlayerDisconnect(f, ++m_seq, tickNumber(), static_cast<uint8_t>(id), reason,
                               static_cast<uint16_t>(engineReason < 0 || engineReason > 0xFFFE ? 0xFFFF : engineReason));
        event(f);
        if (it != m_tracks.end())
            m_tracks.erase(it);
        logf("tick %lu: admin: player %d disconnected (reason %u, engine %d)", m_tick, id, reason, engineReason);
    }

    void onKicked(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        if (!p)
            return;
        std::vector<uint8_t> f;
        encodeKick(f, ++m_seq, tickNumber(), static_cast<uint8_t>(p->getId()), cp1252ToUtf8(p->getName()));
        event(f);
    }

    void onBanned(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        if (!p)
            return;
        const int expiry = argInt(args, argc, 1, 0);
        const int type = argInt(args, argc, 2, 0);
        std::vector<uint8_t> f;
        encodeBan(f, ++m_seq, tickNumber(), static_cast<uint8_t>(p->getId()), cp1252ToUtf8(p->getName()), clampU8(type),
                  static_cast<uint32_t>(expiry < 0 ? 0 : expiry));
        event(f);
    }

    void onSpawn(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        IObject* soldier = argObject(args, argc, 1);
        // spawnPlayer enters the soldier before setIsAlive(true); the same event for a living player
        // is a return to the soldier, not a spawn.
        if (!p || !soldier || p->getIsAlive())
            return;
        IObject* root = rootOf(soldier);
        float pos[3], rot[3];
        placeOf(root, pos, rot);
        std::vector<uint8_t> f;
        encodeSpawn(f, ++m_seq, tickNumber(), static_cast<uint8_t>(p->getId()), clampU8(p->getTeam()), netIdOf(root),
                    objectIdOf(root), cp1252ToUtf8(templateName(root)), pos, rot);
        event(f);
    }

    void onDeath(const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        if (!p)
            return;
        IObject* o = rootOf(argObject(args, argc, 1));
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        if (o)
            copy3(pos, o->getAbsolutePosition());
        std::vector<uint8_t> f;
        encodeDeath(f, ++m_seq, tickNumber(), static_cast<uint8_t>(p->getId()), clampU8(p->getTeam()), netIdOf(o),
                    cp1252ToUtf8(templateName(o)), pos);
        event(f);
    }

    void onRevive(const Arg* args, int argc)
    {
        world::IPlayer* revived = argPlayer(args, argc, 0);
        world::IPlayer* medic = argPlayer(args, argc, 1);
        if (!revived)
            return;
        std::vector<uint8_t> f;
        encodeRevive(f, ++m_seq, tickNumber(), static_cast<uint8_t>(revived->getId()),
                     medic ? static_cast<int16_t>(medic->getId()) : -1);
        event(f);
    }

    // PlayerKilled: (victim, attacker, weapon, assists, vehicle) - GameLogicDamage.cpp; suicides pass
    // (p, p, None, None, None).
    void onKill(const Arg* args, int argc)
    {
        world::IPlayer* victim = argPlayer(args, argc, 0);
        if (!victim)
            return;
        world::IPlayer* attacker = argPlayer(args, argc, 1);
        IObject* weapon = argObject(args, argc, 2);
        IObject* vehicle = argObject(args, argc, 4);
        KillEvent k;
        k.victim = static_cast<uint8_t>(victim->getId());
        k.attacker = attacker ? static_cast<int16_t>(attacker->getId()) : -1;
        if (!attacker)
            k.kind = KillNoAttacker;
        else if (attacker == victim)
            k.kind = KillSuicide;
        else if (!weapon && vehicle)
            k.kind = KillRoad;
        else if (attacker->getTeam() == victim->getTeam())
            k.kind = KillTeam;
        else
            k.kind = KillNormal;
        k.weapon = cp1252ToUtf8(templateName(weapon));
        k.victimTeam = clampU8(victim->getTeam());
        IObject* vroot = rootOf(victim->getVehicle());
        if (vroot && vroot->queryInterface(IID_ICameraObject))
            vroot = 0;
        k.victimRootNetId = netIdOf(vroot);
        k.victimRootTemplate = cp1252ToUtf8(templateName(vroot));
        if (vroot)
            copy3(k.victimPos, vroot->getAbsolutePosition());
        if (attacker)
        {
            k.attackerTeam = clampU8(attacker->getTeam());
            IObject* aroot = vehicle ? rootOf(vehicle) : rootOfPlayer(attacker);
            k.attackerVehicleNetId = netIdOf(aroot);
            k.attackerVehicleTemplate = cp1252ToUtf8(templateName(aroot));
            IObject* apos = rootOfPlayer(attacker) ? rootOfPlayer(attacker) : aroot;
            if (apos)
                copy3(k.attackerPos, apos->getAbsolutePosition());
        }
        const Arg* assists = arg(args, argc, 3);
        if (assists && assists->kind == Arg::Tuple)
        {
            for (int i = 0; i < assists->count; ++i)
            {
                const Arg& pair = args[assists->first + i];
                if (pair.kind != Arg::Tuple || pair.count < 2)
                    continue;
                const Arg& who = args[pair.first];
                const Arg& score = args[pair.first + 1];
                if (who.kind == Arg::Player && who.player)
                    k.assists.push_back(std::make_pair(static_cast<uint8_t>(who.player->getId()),
                                                       score.kind == Arg::Int ? score.i : 0));
            }
        }
        std::vector<uint8_t> f;
        encodeKill(f, ++m_seq, tickNumber(), k);
        event(f);
    }

    // EnterVehicle: (player, object, child soldier active); ExitVehicle: (player, old object).
    void onSeat(uint16_t type, const Arg* args, int argc)
    {
        world::IPlayer* p = argPlayer(args, argc, 0);
        IObject* seatObject = argObject(args, argc, 1);
        if (!p || !seatObject)
            return;
        IObject* root = rootOf(seatObject);
        VehicleSeatEvent e;
        e.player = static_cast<uint8_t>(p->getId());
        IPlayerControlObject* pco =
            static_cast<IPlayerControlObject*>(seatObject->queryInterface(IID_IPlayerControlObject));
        e.seat = pco ? clampU8(pco->getPcoId()) : kSeatNone;
        e.flags = type == EvEnterVehicle && argInt(args, argc, 2, 0) ? 1 : 0;
        e.rootNetId = netIdOf(root);
        e.rootObjectId = objectIdOf(root);
        e.rootTemplate = cp1252ToUtf8(templateName(root));
        e.seatTemplate = cp1252ToUtf8(templateName(seatObject));
        std::vector<uint8_t> f;
        encodeVehicleSeat(f, type, ++m_seq, tickNumber(), e);
        event(f);
    }

    // The engine raises event 24 for every player control object that dies, soldiers included; a
    // soldier's death is already Kill and Death, so only vehicles are reported.
    void onVehicleDestroyed(const Arg* args, int argc)
    {
        IObject* o = rootOf(argObject(args, argc, 0));
        if (!o || o->queryInterface(IID_ISoldier))
            return;
        world::IPlayer* attacker = argPlayer(args, argc, 1);
        float pos[3];
        copy3(pos, o->getAbsolutePosition());
        std::vector<uint8_t> f;
        encodeVehicleDestroyed(f, ++m_seq, tickNumber(), netIdOf(o), objectIdOf(o), cp1252ToUtf8(templateName(o)),
                               attacker ? static_cast<int16_t>(attacker->getId()) : -1, pos);
        event(f);
    }

    // The game-mode script sets the control point's new team in its own handler, after R4, so the
    // event is reported at the module's next tick.
    void onControlPoint(const Arg* args, int argc)
    {
        IObject* o = argObject(args, argc, 0);
        if (!o)
            return;
        PendingCp p;
        p.objectId = o->getObjectId();
        p.object = o;
        p.kind = static_cast<uint8_t>(argInt(args, argc, 1, 0) ? 1 : 0);
        m_cpEvents.push_back(p);
    }

    // ChatMessage: (sender id or -1, text, channel name, flags) - GameServer::handleStringReceivedEvent.
    void onChat(const Arg* args, int argc)
    {
        ChatEvent c;
        const int sender = argInt(args, argc, 0, -1);
        c.channelName = argString(args, argc, 2);
        c.engineFlags = static_cast<uint32_t>(argInt(args, argc, 3, 0));
        c.channel = 255;
        for (size_t k = 0; k < sizeof(kChannelNames) / sizeof(kChannelNames[0]); ++k)
            if (c.channelName == kChannelNames[k])
            {
                c.channel = static_cast<uint8_t>(k);
                break;
            }
        world::IPlayer* p = sender >= 0 ? m_players->getPlayerFromId(sender) : 0;
        c.sender = p ? static_cast<int16_t>(sender) : -1;
        c.team = c.channel == 13 ? clampU8(static_cast<int>(c.engineFlags)) : p ? clampU8(p->getTeam()) : 0;
        std::string text = argString(args, argc, 1);
        if (stripChatPrefixes(text))
            c.textFlags |= 1;
        if (text.size() > kMaxChatText)
            text.resize(kMaxChatText);
        c.text = cp1252ToUtf8(text);
        std::vector<uint8_t> f;
        encodeChat(f, ++m_seq, tickNumber(), c);
        event(f);
    }

    // ---------------------------------------------------------------- commands

    world::IPlayer* player(uint8_t id) { return m_players->getPlayerFromId(id); }

    // The object an ObjectRef names: for a player its root object (vehicle when seated, else the
    // soldier), else the object itself. why receives the reason when it returns null.
    IObject* resolve(const ObjectRef& ref, uint16_t& code, const char*& why)
    {
        code = ErrUnavailable;
        if (ref.kind == RefPlayer)
        {
            world::IPlayer* p = ref.id <= 255 ? player(static_cast<uint8_t>(ref.id)) : 0;
            if (!p)
            {
                code = ErrUnknownPlayer;
                why = "no such player";
                return 0;
            }
            IObject* root = rootOfPlayer(p);
            if (!root)
            {
                code = ErrRefused;
                why = "the player is dead";
            }
            return root;
        }
        IObject* o = 0;
        if (ref.kind == RefNetId)
            o = ref.id && ref.id <= 0xFFFF ? m_game->getObjectFromNetworkableId(static_cast<unsigned short>(ref.id)) : 0;
        else if (ref.kind == RefConsoleId)
            o = m_objects->getObjectFromGrid(ref.id);
        if (!o)
            why = ref.kind == RefNetId ? "no object with that networkable id" : "no object with that console id";
        return o;
    }

    std::string runConsole(const std::string& line)
    {
        std::string out;
        m_console->executeLine(line + "\n", out, false, false, false, false, false);
        return out;
    }

    void onCommand(Command& c)
    {
        if (!c.invalid.empty())
        {
            error(c.requestId, ErrInvalidValue, c.invalid);
            return;
        }
        switch (c.type)
        {
        case MsgRequestSnapshot: sendSnapshot(c.requestId); return;
        case MsgPing:
        {
            std::vector<uint8_t> f;
            encodePing(f, MsgPong, c.requestId, c.clock);
            reply(f);
            return;
        }
        case MsgError:
            logf("tick %lu: admin: WebAdmin reported Error %u for request %u: %s", m_tick, c.index, c.requestId,
                 c.text.substr(0, 200).c_str());
            return;
        case MsgSubscribe: onSubscribe(c); return;
        case MsgUnsubscribe: onUnsubscribe(c); return;
        case CmdConsole: onConsole(c); return;
        case CmdSay: onSay(c); return;
        case CmdTeleport: onTeleport(c); return;
        case CmdSetHealth: onSetHealth(c); return;
        case CmdKillPlayer: onKillPlayer(c); return;
        case CmdSetTeam: onSetTeam(c); return;
        case CmdSetScore: onSetScore(c); return;
        case CmdRank: onRank(c); return;
        case CmdMedal:
        case CmdGameEvent:
        case CmdHudEvent: onRemoteEvent(c); return;
        case CmdKick: onKick(c); return;
        case CmdBan: onBan(c); return;
        case CmdMap: onMap(c); return;
        case CmdListObjects: onListObjects(c); return;
        case CmdSetObjectActive: onSetObjectActive(c); return;
        case CmdSpawnObject: onSpawnObject(c); return;
        case CmdDeleteObject: onDeleteObject(c); return;
        default:
            error(c.requestId, ErrUnknownType, "unexpected message type");
            return;
        }
    }

    void onSubscribe(const Command& c)
    {
        std::vector<std::string> filters;
        for (size_t i = 0; i < c.filters.size(); ++i)
            filters.push_back(lower(c.filters[i]));
        if (c.target == StreamDamageEvents)
            m_damageEvents = true;
        else if (c.target == StreamProjectileEvents)
        {
            m_projectileEvents = true;
            m_projectileEventFilters = filters;
        }
        else
        {
            StreamSub& s = m_streams[c.target];
            s.on = true;
            s.period = static_cast<unsigned int>(std::floor(30.0 / c.value8 + 0.5));
            if (s.period < 1)
                s.period = 1;
            s.due = m_tick + 1;
            s.filters = filters;
        }
        logf("tick %lu: admin: subscribed stream %u at %u Hz (%u filters)", m_tick, c.target, c.value8,
             static_cast<unsigned>(c.filters.size()));
        ok(c.requestId);
    }

    void onUnsubscribe(const Command& c)
    {
        if (c.target == StreamDamageEvents)
            m_damageEvents = false;
        else if (c.target == StreamProjectileEvents)
        {
            m_projectileEvents = false;
            m_projectileEventFilters.clear();
        }
        else if (c.target >= StreamPlayers && c.target <= StreamGame)
            m_streams[c.target] = StreamSub();
        else
        {
            error(c.requestId, ErrInvalidValue, "unknown stream");
            return;
        }
        ok(c.requestId);
    }

    void onConsole(const Command& c)
    {
        if (!m_config.adminAllowConsole)
        {
            error(c.requestId, ErrDisabled, "raw console is disabled (serverext.cfg adminAllowConsole 0)");
            return;
        }
        const std::string out = runConsole(c.text);
        std::vector<std::string> lines;
        bool truncated = false;
        size_t total = 0;
        size_t start = 0;
        while (start < out.size())
        {
            size_t nl = out.find('\n', start);
            if (nl == std::string::npos)
                nl = out.size();
            std::string line = out.substr(start, nl - start);
            if (!line.empty() && line[line.size() - 1] == '\r')
                line.erase(line.size() - 1);
            start = nl + 1;
            const std::string utf8 = cp1252ToUtf8(line, true);
            if (total + utf8.size() > kMaxConsoleOutput || lines.size() >= 0xFFFF)
            {
                truncated = true;
                break;
            }
            total += utf8.size();
            lines.push_back(utf8);
        }
        std::vector<uint8_t> f;
        encodeConsoleResult(f, c.requestId, truncated, lines);
        reply(f);
    }

    void onSay(const Command& c)
    {
        if (c.target == 0)
            m_server->sayAll(c.text);
        else if (c.target == 1)
            m_server->sayTeam(c.player, c.text);
        else
        {
            world::IPlayer* p = player(c.player);
            if (!p)
            {
                error(c.requestId, ErrUnknownPlayer, "no such player");
                return;
            }
            // The engine's Player channel (14): only that player's connection receives it.
            m_server->say(c.text, static_cast<TargetChannel>(14), static_cast<ChannelMessageType>(1), 0,
                          static_cast<unsigned int>(c.player));
        }
        ok(c.requestId);
    }

    void onTeleport(const Command& c)
    {
        uint16_t code;
        const char* why = "";
        IObject* o = resolve(c.ref, code, why);
        if (!o)
        {
            error(c.requestId, code, why);
            return;
        }
        IObject* root = rootOf(o);
        Mat4 t = root->getAbsoluteTransformation();
        if (c.flags & 2)
            matrixFromAttitude(c.rot, t.m);
        if (c.flags & 1)
        {
            t.m[12] = c.pos[0];
            t.m[13] = c.pos[1];
            t.m[14] = c.pos[2];
        }
        root->setAbsoluteTransformation(t);
        if (c.flags & 1)
        {
            // As the bot channel's Teleport: zero the speeds at the new place and wake the body.
            IPhysicsNode* node = static_cast<IPhysicsNode*>(root->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
            if (node)
            {
                node->reset(true);
                node->setIsAwake();
            }
        }
        logf("tick %lu: admin: teleported '%s' (net id %u, console id %u)%s%s", m_tick, templateName(root).c_str(),
             netIdOf(root), objectIdOf(root), c.flags & 1 ? " position" : "", c.flags & 2 ? " rotation" : "");
        ok(c.requestId);
    }

    void onSetHealth(const Command& c)
    {
        uint16_t code;
        const char* why = "";
        IObject* o = resolve(c.ref, code, why);
        if (!o)
        {
            error(c.requestId, code, why);
            return;
        }
        IArmorGLComp* armor = static_cast<IArmorGLComp*>(o->queryComponent(IID_IArmorGLComp, IID_IArmorGLComp));
        if (!armor)
        {
            error(c.requestId, ErrUnavailable, "the object has no armour component");
            return;
        }
        const float hp = (c.flags & 1) ? c.value * armor->getMaxHitPoints() : c.value;
        if (!(hp > 0.0f) && occupied(rootOf(o)))
        {
            error(c.requestId, ErrRefused, "refusing 0 hit points on a soldier or an occupied vehicle (use KillPlayer)");
            return;
        }
        armor->setHitPoints(hp);
        ok(c.requestId);
    }

    void onKillPlayer(const Command& c)
    {
        world::IPlayer* p = player(c.player);
        if (!p)
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return;
        }
        if (!p->getIsAlive())
        {
            error(c.requestId, ErrRefused, "the player is dead");
            return;
        }
        IObject* vehicle = 0;
        if ((c.flags & 1) && !seatOf(p, &vehicle))
            vehicle = 0;
        if (vehicle && countOccupants(vehicle) > 1)
        {
            error(c.requestId, ErrRefused, "the vehicle has other occupants; nothing was done");
            return;
        }
        if (!m_ext->killPlayer(c.player))
        {
            error(c.requestId, ErrRefused, "the engine refused the kill");
            return;
        }
        if (vehicle)
        {
            // Only once the vehicle is empty: zero hit points on an occupied vehicle make the armour
            // evict and destroy the occupant's soldier, which ObjectManager::destroyObject aborts on
            // while the soldier still has its kit.
            IArmorGLComp* armor =
                static_cast<IArmorGLComp*>(vehicle->queryComponent(IID_IArmorGLComp, IID_IArmorGLComp));
            if (!armor || occupied(vehicle))
            {
                error(c.requestId, ErrRefused, "the player was killed but the vehicle was not emptied; it was kept");
                return;
            }
            armor->setHitPoints(0.0f);
        }
        ok(c.requestId);
    }

    void onSetTeam(const Command& c)
    {
        world::IPlayer* p = player(c.player);
        if (!p)
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return;
        }
        p->setTeam(c.value8, false);
        if (p->getTeam() != c.value8)
        {
            error(c.requestId, ErrRefused, "the engine did not change the team");
            return;
        }
        if ((c.flags & 1) && p->getIsAlive())
            m_ext->killPlayer(c.player);
        ok(c.requestId);
    }

    void onSetScore(const Command& c)
    {
        world::IPlayer* p = player(c.player);
        PlayerScoreData* d = p ? p->getScoreData() : 0;
        if (!d)
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return;
        }
        if (c.flags & 1) d->m_score = c.ints[0];
        if (c.flags & 2) d->m_field04 = c.ints[1];
        if (c.flags & 4) d->m_field14 = c.ints[2];
        if (c.flags & 8) d->m_field10 = c.ints[3];
        if (c.flags & 16) d->m_field18 = c.ints[4];
        ok(c.requestId);
    }

    void onRank(const Command& c)
    {
        if (!m_ext->setRank(c.player, c.value8, (c.flags & 1) != 0))
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return;
        }
        ok(c.requestId);
    }

    void onRemoteEvent(const Command& c)
    {
        if (!player(c.player))
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return;
        }
        bool sent = false;
        if (c.type == CmdMedal)
            sent = m_ext->sendMedalEvent(c.player, c.ints[0], c.ints[1]);
        else if (c.type == CmdGameEvent)
            sent = m_ext->sendGameLogicEvent(c.player, c.value8, c.ints[0]);
        else
            sent = m_ext->sendHudEvent(c.player, c.value8, c.ints[0]);
        if (!sent)
        {
            error(c.requestId, ErrRefused, "the engine refused the event");
            return;
        }
        ok(c.requestId);
    }

    // Kick and Ban refuse bots (docs/admin-channel-draft.md: a bot is never banned on address N/A).
    world::IPlayer* kickable(const Command& c)
    {
        world::IPlayer* p = player(c.player);
        if (!p)
        {
            error(c.requestId, ErrUnknownPlayer, "no such player");
            return 0;
        }
        if (p->getIsAIPlayer())
        {
            error(c.requestId, ErrRefused, "bots cannot be kicked or banned; remove bot-channel bots through the bot channel");
            return 0;
        }
        if (!p->getIsConnected())
        {
            error(c.requestId, ErrRefused, "the player is not connected");
            return 0;
        }
        return p;
    }

    void onKick(const Command& c)
    {
        world::IPlayer* p = kickable(c);
        if (!p)
            return;
        m_server->kickPlayer(p->getId(), static_cast<io::PResult>(21), c.text);
        logf("tick %lu: admin: kicked player %d '%s'", m_tick, p->getId(), p->getName().c_str());
        ok(c.requestId);
    }

    void onBan(const Command& c)
    {
        world::IPlayer* p = kickable(c);
        if (!p)
            return;
        char id[8];
        std::snprintf(id, sizeof(id), "%d", p->getId());
        // admin.banPlayer resolves its argument by name first (PlayerManager::getPlayerFromIdString).
        if (m_players->getPlayerFromName(id))
        {
            error(c.requestId, ErrRefused, "a player is named like this player's id; the console cannot tell them apart");
            return;
        }
        if (c.target == 1 && m_ext->getCdKeyHash(p->getId()).empty())
        {
            // An empty key would ban every player without a hash (every player on a LAN server).
            error(c.requestId, ErrRefused, "the server holds no CD-key hash for this player");
            return;
        }
        char time[16];
        if (c.value8 == 0)
            std::snprintf(time, sizeof(time), "perm");
        else if (c.value8 == 1)
            std::snprintf(time, sizeof(time), "round");
        else
            std::snprintf(time, sizeof(time), "%u", c.seconds);
        const std::string line =
            std::string(c.target == 1 ? "admin.banPlayerKey " : "admin.banPlayer ") + id + " " + time;
        const std::string out = runConsole(line);
        logf("tick %lu: admin: '%s' -> '%s' (reason '%s')", m_tick, line.c_str(), out.c_str(), c.text.c_str());
        if (!out.empty() && out.find("not found") != std::string::npos)
        {
            error(c.requestId, ErrUnknownPlayer, cp1252ToUtf8(out));
            return;
        }
        ok(c.requestId);
    }

    void onMap(const Command& c)
    {
        bool done = false;
        if (c.target == 1)
            done = m_ext->restartRound();
        else if (c.target == 2)
            done = m_ext->runMapListEntry(-1);
        else
        {
            IServerExtension::MapListEntry maps[kMaxMaps];
            int current = -1;
            const int count = std::min(m_ext->getMapList(maps, kMaxMaps, current), kMaxMaps);
            int index = -1;
            if (c.target == 3)
                index = c.index < count ? c.index : -1;
            else
                for (int i = 0; i < count && index < 0; ++i)
                    if (lower(maps[i].name) == lower(c.text) && lower(maps[i].gameMode) == lower(c.text2) &&
                        (c.size == 0 || maps[i].size == c.size))
                        index = i;
            if (index < 0)
            {
                error(c.requestId, ErrUnavailable, "no such entry in the map rotation (maplist.list)");
                return;
            }
            done = m_ext->runMapListEntry(index);
        }
        if (!done)
        {
            error(c.requestId, ErrRefused, "the engine refused the map change");
            return;
        }
        logf("tick %lu: admin: map action %u (index %u, '%s' %s)", m_tick, c.target, c.index, c.text.c_str(),
             c.text2.c_str());
        ok(c.requestId);
    }

    void onListObjects(const Command& c)
    {
        IObjectTemplate* t = m_templates->getTemplate(c.text);
        if (!t)
        {
            error(c.requestId, ErrUnavailable, "no such template");
            return;
        }
        std::vector<ObjectInfo> objects;
        bool more = false;
        const std::map<unsigned int, IObject*>& all = m_objects->getAllObjectsFromGrid();
        for (std::map<unsigned int, IObject*>::const_iterator it = all.begin(); it != all.end(); ++it)
        {
            IObject* o = it->second;
            if (!o || o->getTemplate() != t)
                continue;
            if (objects.size() >= c.maxResults)
            {
                more = true;
                break;
            }
            objects.push_back(ObjectInfo());
            ObjectInfo& info = objects.back();
            info.objectId = o->getObjectId();
            info.netId = netIdOf(o);
            info.templateName = cp1252ToUtf8(templateName(o));
            placeOf(o, info.pos, info.rot);
            info.health = healthOf(o, 0);
            info.flags = o->getParent() ? 1 : 0;
        }
        std::vector<uint8_t> f;
        encodeObjectList(f, c.requestId, more, objects);
        reply(f);
    }

    void onSetObjectActive(const Command& c)
    {
        uint16_t code;
        const char* why = "";
        IObject* o = resolve(c.ref, code, why);
        if (!o)
        {
            error(c.requestId, code, why);
            return;
        }
        char line[48];
        std::snprintf(line, sizeof(line), "object.active id%u", o->getObjectId());
        runConsole(line);
        ok(c.requestId);
    }

    void onSpawnObject(const Command& c)
    {
        if (!m_config.adminAllowObjects)
        {
            error(c.requestId, ErrDisabled, "object spawn and delete are disabled (serverext.cfg adminAllowObjects 0)");
            return;
        }
        IObjectTemplate* t = m_templates->getTemplate(c.text);
        if (!t)
        {
            error(c.requestId, ErrUnavailable, "no such template");
            return;
        }
        Vec3 pos, rot;
        pos.x = c.pos[0]; pos.y = c.pos[1]; pos.z = c.pos[2];
        // createObjectOnAllClients builds setRotationZXY(-rot.z, -rot.y, -rot.x): yaw, pitch, roll.
        rot.x = c.rot[0]; rot.y = c.rot[1]; rot.z = c.rot[2];
        IObject* o = m_game->createObjectOnAllClients(t, pos, rot, true, 0, c.value8);
        if (!o)
        {
            error(c.requestId, ErrRefused, "the engine did not create the object");
            return;
        }
        std::vector<uint8_t> f;
        encodeObjectSpawned(f, c.requestId, o->getObjectId(), netIdOf(o));
        reply(f);
        logf("tick %lu: admin: spawned '%s' as console id %u, net id %u at %.1f %.1f %.1f", m_tick, c.text.c_str(),
             o->getObjectId(), netIdOf(o), pos.x, pos.y, pos.z);
    }

    void onDeleteObject(const Command& c)
    {
        if (!m_config.adminAllowObjects)
        {
            error(c.requestId, ErrDisabled, "object spawn and delete are disabled (serverext.cfg adminAllowObjects 0)");
            return;
        }
        uint16_t code;
        const char* why = "";
        IObject* o = resolve(c.ref, code, why);
        if (!o)
        {
            error(c.requestId, code, why);
            return;
        }
        IObject* root = rootOf(o);
        if (o->queryInterface(IID_ISoldier) || o->queryInterface(IID_ICameraObject) || playerOfRoot(root) ||
            root->queryInterface(IID_ISoldier))
        {
            error(c.requestId, ErrRefused, "refusing to delete a soldier, a camera or an occupied vehicle");
            return;
        }
        const unsigned int id = o->getObjectId();
        m_objects->destroyObject(o);
        logf("tick %lu: admin: deleted console id %u", m_tick, id);
        ok(c.requestId);
    }

    struct PendingCp
    {
        unsigned int objectId;
        IObject* object;
        uint8_t kind;
    };

    Config m_config;
    AdminHost* m_host;
    AdminLogFn m_logFn;
    EventManager* m_em;
    IServerExtension* m_ext;
    PlayerManager* m_players;
    ObjectManager* m_objects;
    IGame* m_game;
    IGameServer* m_server;
    System* m_system;
    io::Console* m_console;
    ObjectTemplateManager* m_templates;
    AdminLink* m_link;
    AdminIo* m_io;
    std::string m_describe;

    uint32_t m_session;
    uint32_t m_seq;
    unsigned long m_tick;
    int m_lastStatus;
    int m_lastBeginRound;
    std::deque<Command> m_inbox;
    std::deque<Command> m_pending;
    StreamSub m_streams[5];       // indexed by stream 1..4
    std::map<int, Track> m_tracks;
    std::vector<int> m_seen;
    std::vector<ControlPoint> m_cps;
    std::vector<PendingCp> m_cpEvents;
    bool m_cpsScanned;
    unsigned long m_lastCpScan;
    std::vector<Projectile> m_projectiles;
    std::vector<ProjectileState> m_projectileStates;
    bool m_damageEvents;
    bool m_projectileEvents;
    std::vector<std::string> m_projectileEventFilters;
    bool m_r4;

    std::vector<IObject*> m_vehicleRoots;
    std::vector<VehicleState> m_vehicles;
    std::vector<uint8_t> m_ids;
    std::vector<PlayerState> m_states;
    std::vector<uint8_t> m_frame;

    uint64_t m_eventsSent, m_eventsDropped, m_commands;
    double m_bytes[kCategories];
    Histogram m_cost;
};

AdminLogFn Admin::s_logFn = 0;

} // namespace

AdminChannel* startAdminChannel(const Config& config, ClassManager* classManager, AdminHost* host, AdminLogFn log,
                                std::string& error)
{
    // Never destroyed: the engine keeps the listener and observer pointers.
    Admin* a = new Admin(config, host, log);
    if (!a->init(classManager, error))
        return 0;   // leaked on purpose: the I/O thread may already hold it
    return a;
}

} // namespace serverext
