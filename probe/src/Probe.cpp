// [TEMP] serverext_probe: smoke module for the OpenBF2 R1/R1b engine changes, the Q1 per-tick hook
// and the R2 heli-entry route. Not the serverext module: no socket, no protocol, one hard-coded bot.
//
// What it does, all on the main thread inside the EventManager category-7 tick event:
//   1. Counts ticks (Q1).
//   2. Once the map is loaded, finds the team-1 control point and the non-enter-on-spawn spawn points
//      near it, and takes their spawn group id (the "walk spawn points" route).
//   3. createInputPlayer("[BOT] probe") through IServerExtension (R1); team 1, kit 0, that group.
//   4. Waits until the engine spawns it, then enters the pilot seat of the nearest free heli within
//      30 m with IGame::enterVehicle(player, seat, false) (R2).
//   5. Writes a fixed collective with the mapping bits set and hundredths quantization.
//   6. Logs once per second: tick, bot state, root object transform/velocity (Q2 conversion).
// If the bot disappears (map change), it logs that and starts again from step 2 without destroying
// anything itself.
//
// Only typed virtual calls are made into the engine; bf2 exports no symbols to modules.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Core/ClassManager.h"
#include "Core/EventManager.h"
#include "Core/Ids.h"
#include "Game/IEventListener.h"
#include "Game/IGame.h"
#include "Game/GameServer/IServerExtension.h"
#include "Game/Objects/IObject.h"
#include "Game/Objects/IObjectTemplate.h"
#include "Game/Objects/IPlayer.h"
#include "Game/Objects/IPlayerControlObjectTemplate.h"
#include "Game/Objects/ISpawnPoint.h"
#include "Game/Objects/IControlPoint.h"
#include "IO/Input/PlayerInput.h"
#include "Physics/IPhysicsNode.h"
#include "Scene/Object/ObjectManager.h"
#include "Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#include "Scene/Object/PlayerManager.h"

using namespace dice::hfe;
using namespace dice::hfe::world;   // the IID_* constants and the object interfaces

namespace {

const EventCategory kTickCategory = static_cast<EventCategory>(7);
const int kTeam = 1;
const int kKit = 0;
const float kBaseRadius = 60.0f;   // spawn points this close to the team's base control point
const float kHeliRadius = 30.0f;
const int kVCHelicopter = 3;
const int kTicksPerSecond = 30;

void plog(const char* fmt, ...)
{
    std::fprintf(stderr, "[TEMP][serverext_probe] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

float dist(const Math::Vec3& a, const Math::Vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

world::IObject* rootOf(world::IObject* o)
{
    if (!o)
        return 0;
    while (o->getParent())
        o = o->getParent();
    return o;
}

const char* rootTemplate(world::IObject* o)
{
    return o && o->getTemplate() ? o->getTemplate()->getName().c_str() : "?";
}

// The contract's SetInput quantization: clamp, then truncate to hundredths.
float quantize(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<float>(static_cast<short>(static_cast<int>(v * 100.0f))) / 100.0f;
}

// The mask PlayerAction::get produces: axes 0..5 and the button table (PlayerAction.cpp:13-18).
unsigned long long mappedMask()
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

enum State { FindBase, WaitAlive, EnterHeli, Fly };

const char* stateName(State s)
{
    switch (s)
    {
    case FindBase:  return "FindBase";
    case WaitAlive: return "WaitAlive";
    case EnterHeli: return "EnterHeli";
    case Fly:       return "Fly";
    }
    return "?";
}

class Probe : public IEventListener
{
public:
    Probe() : m_cm(0), m_em(0), m_ext(0), m_players(0), m_objects(0), m_game(0),
              m_tick(0), m_state(FindBase), m_spawnGroup(0), m_botId(-1), m_stateTick(0),
              m_flyStartTick(0), m_bots(0), m_enterAttempts(0), m_lastPhysicsNull(false) {}

    bool init(ClassManager* cm)
    {
        m_cm = cm;
        m_em = reinterpret_cast<EventManager*>(cm->getSingleton("EventManager"));
        m_ext = reinterpret_cast<IServerExtension*>(cm->getSingleton("OpenBF2ServerExtension"));
        m_players = reinterpret_cast<world::PlayerManager*>(cm->getSingleton("PlayerManager"));
        m_objects = reinterpret_cast<world::ObjectManager*>(cm->getSingleton("ObjectManager"));
        m_game = reinterpret_cast<IGame*>(cm->getSingleton("Game"));
        plog("initDll: EventManager=%p OpenBF2ServerExtension=%p PlayerManager=%p ObjectManager=%p Game=%p",
             (void*)m_em, (void*)m_ext, (void*)m_players, (void*)m_objects, (void*)m_game);
        if (!m_em || !m_ext || !m_players || !m_objects || !m_game)
        {
            plog("initDll: a required singleton is missing; refusing to load");
            return false;
        }
        const bool registered = m_em->registerEventHandler(kTickCategory, this, 0);
        const unsigned int handle = m_em->postEventNextUpdate(kTickCategory, 1, 0);
        plog("initDll: registerEventHandler(7)=%d first postEventNextUpdate handle=%u",
             registered ? 1 : 0, handle);
        return true;
    }

    virtual void handleEvent(EventCategory category, unsigned int id, EventNode*, void*)
    {
        if (category != kTickCategory || id != 1)
            return;
        ++m_tick;
        step();
        if (m_tick % kTicksPerSecond == 0)
            report();
        m_em->postEventNextUpdate(kTickCategory, 1, 0);
    }

private:
    void setState(State s)
    {
        plog("tick %lu: state %s -> %s", m_tick, stateName(m_state), stateName(s));
        m_state = s;
        m_stateTick = m_tick;
    }

    world::IPlayer* bot() const
    {
        if (m_botId < 0)
            return 0;
        return m_players->getPlayerFromId(m_botId);
    }

    void step()
    {
        if (m_state != FindBase && !bot())
        {
            plog("tick %lu: bot id %d no longer exists (engine removed it); not destroying it again",
                 m_tick, m_botId);
            m_botId = -1;
            m_spawnGroup = 0;
            setState(FindBase);
            return;
        }

        switch (m_state)
        {
        case FindBase:
            if (m_tick % kTicksPerSecond == 0)
                findBaseAndCreate();
            break;
        case WaitAlive:
            waitAlive();
            break;
        case EnterHeli:
            enterHeli();
            break;
        case Fly:
            fly();
            break;
        }
    }

    // Steps 2 and 3. Spawn points carry no team; the team comes from their control point, which
    // ISpawnPoint does not expose, so this pairs them with the team's control point by distance.
    void findBaseAndCreate()
    {
        const std::map<unsigned int, world::IObject*>& all = m_objects->getAllRegisteredObjects();
        std::vector<world::IObject*> controlPoints;
        std::vector<world::IObject*> spawnPoints;
        for (std::map<unsigned int, world::IObject*>::const_iterator it = all.begin(); it != all.end(); ++it)
        {
            world::IObject* o = it->second;
            if (!o)
                continue;
            if (o->queryInterface(IID_IControlPoint))
                controlPoints.push_back(o);
            if (o->queryInterface(IID_ISpawnPoint))
                spawnPoints.push_back(o);
        }
        if (controlPoints.empty() || spawnPoints.empty())
            return;   // map not loaded yet

        world::IObject* base = 0;
        for (size_t i = 0; i < controlPoints.size(); ++i)
        {
            world::IControlPoint* cp = static_cast<world::IControlPoint*>(controlPoints[i]->queryInterface(IID_IControlPoint));
            const Math::Vec3& p = controlPoints[i]->getAbsolutePosition();
            if (m_bots == 0)
                plog("  control point '%s' team %d at %.1f %.1f %.1f", controlPoints[i]->getName().c_str(),
                     cp->getTeam(), p.x, p.y, p.z);
            if (cp->getTeam() == kTeam && !base)
                base = controlPoints[i];
        }
        if (!base)
        {
            plog("tick %lu: %u objects, %u control points, %u spawn points, none of team %d yet",
                 m_tick, (unsigned)all.size(), (unsigned)controlPoints.size(),
                 (unsigned)spawnPoints.size(), kTeam);
            return;
        }

        int group = 0;
        for (size_t i = 0; i < spawnPoints.size(); ++i)
        {
            ISpawnPoint* sp = static_cast<ISpawnPoint*>(spawnPoints[i]->queryInterface(IID_ISpawnPoint));
            const Math::Vec3& p = spawnPoints[i]->getAbsolutePosition();
            const float d = dist(p, base->getAbsolutePosition());
            const bool eos = sp->getEnterOnSpawn();
            if (m_bots == 0)
                plog("  spawn point '%s' group %d enterOnSpawn %d at %.1f %.1f %.1f (%.1f m from base)",
                     spawnPoints[i]->getName().c_str(), sp->getSpawnGroupId(), eos ? 1 : 0,
                     p.x, p.y, p.z, d);
            if (d <= kBaseRadius && !eos && !group)
                group = sp->getSpawnGroupId();
        }
        plog("tick %lu: %u objects, %u control points, %u spawn points; base '%s', on-foot group %d",
             m_tick, (unsigned)all.size(), (unsigned)controlPoints.size(), (unsigned)spawnPoints.size(),
             base->getName().c_str(), group);
        if (group <= 0)
            return;

        m_spawnGroup = group;
        m_botId = m_ext->createInputPlayer("[BOT] probe");
        world::IPlayer* p = bot();
        plog("tick %lu: createInputPlayer -> %d (player %p, ai %d remote %d connected %d)", m_tick, m_botId,
             (void*)p, p ? p->getIsAIPlayer() : -1, p ? p->getIsRemote() : -1, p ? p->getIsConnected() : -1);
        if (!p)
        {
            m_botId = -1;
            return;
        }
        ++m_bots;
        p->setTeam(kTeam, false);
        p->setKit(kKit);
        p->setSpawnGroup(m_spawnGroup);
        plog("tick %lu: bot %d team %d kit %d spawnGroup %d; AI input buffer %p", m_tick, m_botId,
             p->getTeam(), p->getKit(), p->getSpawnGroup(), p->getAIPlayerInput());
        setState(WaitAlive);
    }

    void waitAlive()
    {
        world::IPlayer* p = bot();
        if (p->getIsAlive())
        {
            world::IObject* root = rootOf(p->getVehicle());
            const Math::Vec3& pos = root ? root->getAbsolutePosition() : Math::Vec3();
            plog("tick %lu: bot %d is alive after %lu ticks, at %.1f %.1f %.1f", m_tick, m_botId,
                 m_tick - m_stateTick, pos.x, pos.y, pos.z);
            m_enterAttempts = 0;
            setState(EnterHeli);
            return;
        }
        // uPlayingSpawning clears a stale group; put it back, rate-limited.
        if (p->getSpawnGroup() == 0 && (m_tick - m_stateTick) % (5 * kTicksPerSecond) == 0)
        {
            plog("tick %lu: bot spawn group was cleared; setting %d again", m_tick, m_spawnGroup);
            p->setSpawnGroup(m_spawnGroup);
        }
    }

    // Step 4. One attempt per second after a one-second settle.
    void enterHeli()
    {
        if (m_tick - m_stateTick < (unsigned long)kTicksPerSecond || (m_tick - m_stateTick) % kTicksPerSecond)
            return;
        world::IPlayer* p = bot();
        if (!p->getIsAlive())
        {
            setState(WaitAlive);
            return;
        }
        world::IObject* me = rootOf(p->getVehicle());
        if (!me)
            return;
        const Math::Vec3& myPos = me->getAbsolutePosition();

        const std::map<unsigned int, world::IObject*>& all = m_objects->getAllRegisteredObjects();
        world::IObject* bestSeat = 0;
        float bestDist = kHeliRadius;
        for (std::map<unsigned int, world::IObject*>::const_iterator it = all.begin(); it != all.end(); ++it)
        {
            world::IObject* o = it->second;
            if (!o || o->getParent())
                continue;
            world::IPlayerControlObject* pco =
                static_cast<world::IPlayerControlObject*>(o->queryInterface(IID_IPlayerControlObject));
            if (!pco || !o->getTemplate())
                continue;
            world::IPlayerControlObjectTemplate* pt = static_cast<world::IPlayerControlObjectTemplate*>(
                const_cast<world::IObjectTemplate*>(o->getTemplate())->queryInterface(IID_IPlayerControlObjectTemplate));
            if (!pt || pt->getVehicleCategory() != kVCHelicopter)
                continue;
            const float d = dist(o->getAbsolutePosition(), myPos);
            std::map<int, world::IPlayerControlObject*>* seats = pco->getPcos();
            world::IPlayerControlObject* pilot = 0;
            if (seats)
            {
                std::map<int, world::IPlayerControlObject*>::iterator s = seats->find(0);
                if (s != seats->end())
                    pilot = s->second;
            }
            const int team = pco->getTeam();
            plog("  heli '%s' (%s) %.1f m away, team %d, seats %u, pilot %p occupied %d",
                 o->getName().c_str(), o->getTemplate()->getName().c_str(), d, team,
                 seats ? (unsigned)seats->size() : 0u, (void*)pilot, pilot ? pilot->isOccupied() : -1);
            if (!pilot || pilot->isOccupied() || (team != 0 && team != p->getTeam()))
                continue;
            if (d < bestDist)
            {
                bestDist = d;
                bestSeat = static_cast<world::IObject*>(pilot->queryInterface(IID_IObject));
            }
        }
        if (!bestSeat)
        {
            if (++m_enterAttempts % 10 == 1)
                plog("tick %lu: no free heli pilot seat within %.0f m of %.1f %.1f %.1f", m_tick,
                     kHeliRadius, myPos.x, myPos.y, myPos.z);
            return;
        }
        const bool ok = m_game->enterVehicle(p, bestSeat, false);
        world::IObject* now = p->getVehicle();
        plog("tick %lu: enterVehicle(bot %d, seat %p, false) -> %d; player vehicle now %p, root %p",
             m_tick, m_botId, (void*)bestSeat, ok ? 1 : 0, (void*)now, (void*)rootOf(now));
        if (now == bestSeat)
        {
            m_flyStartTick = m_tick;
            setState(Fly);
        }
    }

    // Step 5: 3 s idle, then full collective until 60 m above the pad, then collective 0.
    float collective() const
    {
        const unsigned long t = m_tick - m_flyStartTick;
        if (t < 3UL * kTicksPerSecond)
            return 0.0f;
        return m_climbDone ? 0.0f : 1.0f;
    }

    void fly()
    {
        world::IPlayer* p = bot();
        io::PlayerInput* in = static_cast<io::PlayerInput*>(p->getAIPlayerInput());
        if (!in)
            return;
        world::IObject* root = rootOf(p->getVehicle());
        if (!p->getIsAlive() || !root || root->queryInterface(IID_ISoldier))
        {
            // Died with the heli, or no longer seated: neutral input, then start over.
            for (int i = 0; i < 64; ++i)
                in->inputs[i] = 0.0f;
            plog("tick %lu: bot left the heli (alive %d); neutral input, back to WaitAlive", m_tick,
                 p->getIsAlive() ? 1 : 0);
            m_climbDone = false;
            m_padY = 0.0f;
            setState(WaitAlive);
            return;
        }
        if (root)
        {
            if (m_padY == 0.0f && m_tick == m_flyStartTick + 1)
                m_padY = root->getAbsolutePosition().y;
            if (!m_climbDone && root->getAbsolutePosition().y > m_padY + 60.0f)
            {
                m_climbDone = true;
                plog("tick %lu: 60 m above the pad; collective -> 0", m_tick);
            }
        }
        in->mappingFlags = mappedMask();
        for (int i = 0; i < 64; ++i)
            in->inputs[i] = 0.0f;
        in->inputs[io::PIPitch] = quantize(0.0f);
        in->inputs[io::PIRoll] = quantize(0.0f);
        in->inputs[io::PIThrottle] = quantize(collective());
        in->inputs[io::PIYaw] = quantize(0.0f);
    }

    void report()
    {
        world::IPlayer* p = bot();
        if (!p)
        {
            plog("tick %lu state %s: no bot (players %d, alive humans %d)", m_tick, stateName(m_state),
                 m_players->getNumberOfPlayers(), m_players->getNumberOfAliveHumanPlayers());
            return;
        }
        world::IObject* vehicle = p->getVehicle();
        world::IObject* root = rootOf(vehicle);
        int seat = 255;
        if (vehicle && root && !root->queryInterface(IID_ISoldier))
        {
            world::IPlayerControlObject* pco =
                static_cast<world::IPlayerControlObject*>(vehicle->queryInterface(IID_IPlayerControlObject));
            seat = pco ? pco->getPcoId() : 254;
        }
        char where[512] = "no root object";
        if (root)
        {
            const Mat4& m = root->getAbsoluteTransformation();
            const float r2d = 57.2957795f;
            float s = m.m[9];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            const float yaw = std::atan2(m.m[8], m.m[10]) * r2d;
            const float pitch = -std::asin(s) * r2d;
            const float roll = std::atan2(m.m[1], m.m[5]) * r2d;
            // The contract names queryInterface(IID_IPhysicsNode); the engine stores the node as a
            // component (setComponent(IID_IPhysicsNode, node)), so the typed route is
            // queryComponent(IID_IPhysicsNode, IID_IPhysicsNode), IObject slot 23. Log both.
            void* viaInterface = root->queryInterface(IID_IPhysicsNode);
            world::IPhysicsNode* node =
                static_cast<world::IPhysicsNode*>(root->queryComponent(IID_IPhysicsNode, IID_IPhysicsNode));
            const bool interfaceNull = (viaInterface == 0);
            if (interfaceNull != m_lastPhysicsNull || m_tick == kTicksPerSecond)
                plog("tick %lu: root '%s' queryInterface(IID_IPhysicsNode)=%p queryComponent(IID_IPhysicsNode)=%p",
                     m_tick, rootTemplate(root), viaInterface, (void*)node);
            m_lastPhysicsNull = interfaceNull;
            Math::Vec3 v, w;
            v.x = v.y = v.z = 0.0f;
            w.x = w.y = w.z = 0.0f;
            if (node)
            {
                v = node->getPositionalSpeed();
                w = node->getRotationalSpeed();
            }
            std::snprintf(where, sizeof(where),
                          "root '%s' pos %.2f %.2f %.2f ypr %.1f %.1f %.1f vel %.2f %.2f %.2f angvel %.3f %.3f %.3f%s",
                          rootTemplate(root), m.m[12], m.m[13], m.m[14], yaw, pitch, roll,
                          v.x, v.y, v.z, w.x, w.y, w.z, node ? "" : " (no physics node)");
        }
        const io::PlayerInput* in = static_cast<const io::PlayerInput*>(p->getAIPlayerInput());
        plog("tick %lu state %s bot %d alive %d seat %d team %d group %d collective %.2f | %s", m_tick,
             stateName(m_state), m_botId, p->getIsAlive() ? 1 : 0, seat, p->getTeam(), p->getSpawnGroup(),
             in ? in->inputs[io::PIThrottle] : -9.0f, where);
    }

    ClassManager* m_cm;
    EventManager* m_em;
    IServerExtension* m_ext;
    world::PlayerManager* m_players;
    world::ObjectManager* m_objects;
    IGame* m_game;
    unsigned long m_tick;
    State m_state;
    int m_spawnGroup;
    int m_botId;
    unsigned long m_stateTick;
    unsigned long m_flyStartTick;
    int m_bots;
    int m_enterAttempts;
    bool m_lastPhysicsNull;
    bool m_climbDone = false;
    float m_padY = 0.0f;
};

Probe g_probe;

} // namespace

extern "C" __attribute__((visibility("default"))) bool initDll(ClassManager* classManager)
{
    plog("loaded; initDll(%p)", (void*)classManager);
    return classManager && g_probe.init(classManager);
}
