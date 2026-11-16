#include "AdminProtocol.h"

#include <cmath>
#include <cstring>

#include "SeatOrder.h"
#include "Vote.h"

namespace serverext {
namespace admin {

using proto::Reader;
using proto::Writer;

const char* const kWebAdminProofLabel = "OBF2-ADMIN-1 webadmin";
const char* const kServerProofLabel = "OBF2-ADMIN-1 server";

namespace {

void zero3(float v[3]) { v[0] = v[1] = v[2] = 0.0f; }

const size_t kMaxSayText = 1024;
const size_t kMaxReason = 256;
const size_t kMaxTemplateName = 256;
const size_t kMaxFilters = 64;

bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The bot channel's world bounds (docs/protocol.md, Teleport).
bool inWorld(const float p[3])
{
    return std::fabs(p[0]) <= 8192.0f && std::fabs(p[2]) <= 8192.0f && p[1] >= -1000.0f && p[1] <= 10000.0f;
}

void readRef(Reader& r, ObjectRef& ref)
{
    ref.kind = r.u8();
    ref.id = r.u32();
}

bool bad(std::string* badValue, const char* why)
{
    if (badValue)
        *badValue = why;
    return false;
}

// Converts an inbound UTF-8 string to engine bytes and checks it for control bytes and length.
bool engineText(const std::string& in, size_t maxBytes, bool allowEmpty, std::string& out, std::string* badValue,
                const char* what)
{
    if (in.size() > maxBytes)
        return bad(badValue, what);
    if (!allowEmpty && in.empty())
        return bad(badValue, what);
    if (hasControlBytes(in))
        return bad(badValue, what);
    if (!utf8ToCp1252(in, out))
        return bad(badValue, what);
    return true;
}

// cp1252 0x80..0x9F; 0 marks the five undefined bytes, which map to the C1 code point itself.
const uint16_t kCp1252High[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

PlayerState::PlayerState()
    : team(0), squad(0), life(kLifeDead), flags(0), ping(0), rootNetId(0), rootObjectId(kNoObjectId), seat(kSeatNone),
      health(-1.0f), viewMode(kViewModeNone), lagMs(0)
{
    zero3(pos); zero3(rot); zero3(vel);
}

VehicleState::VehicleState()
    : netId(0), objectId(kNoObjectId), team(0), flags(0), health(-1.0f), seats(0), occupied(0)
{
    zero3(pos); zero3(rot); zero3(vel);
}

ControlPointState::ControlPointState()
    : objectId(kNoObjectId), netId(0), team(0), flagTeam(0), takeOver(0.0f), capturing(0)
{
    zero3(pos);
}

ProjectileState::ProjectileState()
    : id(0), owner(-1), team(0), flags(0), targetNetId(0), timeToLive(0.0f)
{
    zero3(pos); zero3(rot); zero3(vel);
}

AimState::AimState() : player(0), flags(0)
{
    zero3(cameraPos); zero3(cameraRot); zero3(weaponPos); zero3(weaponRot);
}

WeaponFireEvent::WeaponFireEvent() : shooter(-1), rootNetId(0), rounds(0)
{
    zero3(origin); zero3(velocity);
}

ProjectileCollisionEvent::ProjectileCollisionEvent()
    : id(0), shooter(-1), kind(kCollisionTerrain), flags(0), rootNetId(0), rootObjectId(kNoObjectId), partPlayer(-1),
      material(0), latency(0.0f)
{
    zero3(pos); zero3(normal); zero3(speed); zero3(partPos); zero3(partRot); zero3(rootPos); zero3(rootRot);
}

ProjectileTraceEvent::ProjectileTraceEvent() : id(0), shooter(-1), firstTick(0), flags(0) {}

ObjectInfo::ObjectInfo() : objectId(kNoObjectId), netId(0), health(-1.0f), flags(0)
{
    zero3(pos); zero3(rot);
}

KillEvent::KillEvent()
    : victim(0), attacker(-1), kind(KillNoAttacker), victimTeam(0), victimRootNetId(0), attackerTeam(0),
      attackerVehicleNetId(0)
{
    zero3(victimPos); zero3(attackerPos);
}

DamageEvent::DamageEvent()
    : victimPlayer(-1), victimRootNetId(0), victimRootObjectId(kNoObjectId), attacker(-1), amount(0), before(0), after(0),
      max(0), flags(0)
{
    zero3(pos);
}

ProjectileHitEvent::ProjectileHitEvent() : id(0), owner(-1), flags(0), firstVictimNetId(0)
{
    zero3(pos);
}

Command::Command()
    : type(0), session(0), requestId(0), player(0), target(0), flags(0), value8(0), seconds(0), index(0), size(0),
      maxResults(0), clock(0), value(0.0f), instance(0), target16(-1), netId(0)
{
    for (int i = 0; i < 5; ++i)
        ints[i] = 0;
    for (int i = 0; i < 9; ++i)
        points[i] = 0.0f;
    zero3(pos); zero3(rot);
}

bool hudEventRawAllowed(uint8_t id)
{
    static const uint8_t kAllowed[] = { 40, 41, 42, 43, 44, 51, 64, 65, 66, 70, 71, 74, 83 };
    for (size_t i = 0; i < sizeof(kAllowed); ++i)
        if (kAllowed[i] == id)
            return true;
    return false;
}

// ---------------------------------------------------------------- structures

void writeVec3(Writer& w, const float v[3])
{
    w.f32(v[0]); w.f32(v[1]); w.f32(v[2]);
}

void readVec3(Reader& r, float v[3])
{
    v[0] = r.f32(); v[1] = r.f32(); v[2] = r.f32();
}

void writeGameState(Writer& w, const GameState& g)
{
    w.u8(g.status);
    w.u8(g.beginRound);
    w.f32(g.timeToNext);
    w.i32(g.tickets[0]);
    w.i32(g.tickets[1]);
    w.f32(g.roundTime);
    w.f32(g.timeLimit);
    w.u8(g.winner);
    w.u8(g.victoryType);
}

void readGameState(Reader& r, GameState& g)
{
    g.status = r.u8();
    g.beginRound = r.u8();
    g.timeToNext = r.f32();
    g.tickets[0] = r.i32();
    g.tickets[1] = r.i32();
    g.roundTime = r.f32();
    g.timeLimit = r.f32();
    g.winner = r.u8();
    g.victoryType = r.u8();
}

void writePlayerState(Writer& w, const PlayerState& s)
{
    w.u8(s.team);
    w.u8(s.squad);
    w.u8(s.life);
    w.u8(s.flags);
    w.u16(s.ping);
    w.u32(s.rootNetId);
    w.u32(s.rootObjectId);
    w.u8(s.seat);
    writeVec3(w, s.pos);
    writeVec3(w, s.rot);
    writeVec3(w, s.vel);
    w.f32(s.health);
    w.u8(s.viewMode);
    w.u16(s.lagMs);
}

void readPlayerState(Reader& r, PlayerState& s)
{
    s.team = r.u8();
    s.squad = r.u8();
    s.life = r.u8();
    s.flags = r.u8();
    s.ping = r.u16();
    s.rootNetId = r.u32();
    s.rootObjectId = r.u32();
    s.seat = r.u8();
    readVec3(r, s.pos);
    readVec3(r, s.rot);
    readVec3(r, s.vel);
    s.health = r.f32();
    s.viewMode = r.u8();
    s.lagMs = r.u16();
}

void writePlayerScore(Writer& w, const PlayerScore& s)
{
    w.i32(s.score);
    w.i32(s.teamScore);
    w.i32(s.kills);
    w.i32(s.deaths);
    w.i32(s.teamKills);
    w.i16(s.rank);
}

void readPlayerScore(Reader& r, PlayerScore& s)
{
    s.score = r.i32();
    s.teamScore = r.i32();
    s.kills = r.i32();
    s.deaths = r.i32();
    s.teamKills = r.i32();
    s.rank = r.i16();
}

void writePlayerInfo(Writer& w, const PlayerInfo& p)
{
    w.u8(p.id);
    w.str(p.name);
    w.u8(p.flags);
    w.i32(p.profileId);
    w.str(p.ip);
    w.str(p.hash);
    writePlayerScore(w, p.score);
    writePlayerState(w, p.state);
}

void readPlayerInfo(Reader& r, PlayerInfo& p)
{
    p.id = r.u8();
    r.str(p.name);
    p.flags = r.u8();
    p.profileId = r.i32();
    r.str(p.ip);
    r.str(p.hash);
    readPlayerScore(r, p.score);
    readPlayerState(r, p.state);
}

void writeVehicleState(Writer& w, const VehicleState& v)
{
    w.u32(v.netId);
    w.u32(v.objectId);
    w.str(v.templateName);
    w.u8(v.team);
    w.u8(v.flags);
    writeVec3(w, v.pos);
    writeVec3(w, v.rot);
    writeVec3(w, v.vel);
    w.f32(v.health);
    w.u8(v.seats);
    w.u32(v.occupied);
}

void readVehicleState(Reader& r, VehicleState& v)
{
    v.netId = r.u32();
    v.objectId = r.u32();
    r.str(v.templateName);
    v.team = r.u8();
    v.flags = r.u8();
    readVec3(r, v.pos);
    readVec3(r, v.rot);
    readVec3(r, v.vel);
    v.health = r.f32();
    v.seats = r.u8();
    v.occupied = r.u32();
}

void writeControlPointState(Writer& w, const ControlPointState& c)
{
    w.u32(c.objectId);
    w.u32(c.netId);
    w.str(c.templateName);
    w.u8(c.team);
    w.u8(c.flagTeam);
    w.f32(c.takeOver);
    w.u8(c.capturing);
    writeVec3(w, c.pos);
}

void readControlPointState(Reader& r, ControlPointState& c)
{
    c.objectId = r.u32();
    c.netId = r.u32();
    r.str(c.templateName);
    c.team = r.u8();
    c.flagTeam = r.u8();
    c.takeOver = r.f32();
    c.capturing = r.u8();
    readVec3(r, c.pos);
}

void writeProjectileState(Writer& w, const ProjectileState& p)
{
    w.u32(p.id);
    w.str(p.templateName);
    w.i16(p.owner);
    w.u8(p.team);
    w.u8(p.flags);
    w.u32(p.targetNetId);
    writeVec3(w, p.pos);
    writeVec3(w, p.rot);
    writeVec3(w, p.vel);
    w.f32(p.timeToLive);
}

void readProjectileState(Reader& r, ProjectileState& p)
{
    p.id = r.u32();
    r.str(p.templateName);
    p.owner = r.i16();
    p.team = r.u8();
    p.flags = r.u8();
    p.targetNetId = r.u32();
    readVec3(r, p.pos);
    readVec3(r, p.rot);
    readVec3(r, p.vel);
    p.timeToLive = r.f32();
}

// ---------------------------------------------------------------- session

std::vector<uint8_t> proofTranscript(const char* label, const std::string& serverId, const uint8_t moduleNonce[kNonceSize],
                                     const uint8_t webAdminNonce[kNonceSize])
{
    std::vector<uint8_t> t;
    t.insert(t.end(), label, label + std::strlen(label));
    t.push_back(0);
    const size_t n = serverId.size() > 0xFFFF ? 0xFFFF : serverId.size();
    t.push_back(static_cast<uint8_t>(n));
    t.push_back(static_cast<uint8_t>(n >> 8));
    t.insert(t.end(), serverId.begin(), serverId.begin() + n);
    t.insert(t.end(), moduleNonce, moduleNonce + kNonceSize);
    t.insert(t.end(), webAdminNonce, webAdminNonce + kNonceSize);
    return t;
}

void encodeHello(std::vector<uint8_t>& out, const std::string& serverId, const uint8_t nonce[kNonceSize],
                 const std::string& build)
{
    Writer w(out);
    w.begin(MsgHello);
    w.u16(kVersion);
    w.str(serverId);
    w.bytes(nonce, kNonceSize);
    w.str(build);
    w.finish();
}

bool parseChallenge(const uint8_t* p, size_t n, ChallengeMsg& out)
{
    Reader r(p, n);
    r.bytes(out.nonce, kNonceSize);
    r.bytes(out.proof, kProofSize);
    return r.done();
}

void encodeAuth(std::vector<uint8_t>& out, const uint8_t proof[kProofSize])
{
    Writer w(out);
    w.begin(MsgAuth);
    w.bytes(proof, kProofSize);
    w.finish();
}

void encodeWelcome(std::vector<uint8_t>& out, const WelcomeInfo& i)
{
    Writer w(out);
    w.begin(MsgWelcome);
    w.u16(kVersion);
    w.u32(i.session);
    w.u32(i.tick);
    w.f32(i.tickRate);
    w.str(i.serverName);
    w.u16(i.gamePort);
    w.u16(i.gameSpyPort);
    w.u8(i.maxPlayers);
    w.u8(i.capabilities);
    w.str(i.map);
    w.str(i.mode);
    w.u16(i.mapSize);
    w.u16(i.mapIndex);
    const size_t n = i.maps.size() > 0xFFFF ? 0xFFFF : i.maps.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t k = 0; k < n; ++k)
    {
        w.str(i.maps[k].name);
        w.str(i.maps[k].mode);
        w.u16(i.maps[k].size);
    }
    w.str(i.team1);
    w.str(i.team2);
    writeGameState(w, i.game);
    w.finish();
}

// ---------------------------------------------------------------- state

void encodeSnapshot(std::vector<uint8_t>& out, uint32_t requestId, uint32_t lastSeq, uint32_t tick, double time,
                    const GameState& game, const std::vector<PlayerInfo>& players,
                    const std::vector<VehicleState>& vehicles, const std::vector<ControlPointState>& controlPoints,
                    const std::vector<ProjectileState>& projectiles)
{
    Writer w(out);
    w.begin(MsgSnapshot);
    w.u32(requestId);
    w.u32(lastSeq);
    w.u32(tick);
    w.f64(time);
    writeGameState(w, game);
    const size_t np = players.size() > 255 ? 255 : players.size();
    w.u8(static_cast<uint8_t>(np));
    for (size_t k = 0; k < np; ++k)
        writePlayerInfo(w, players[k]);
    const size_t nv = vehicles.size() > 0xFFFF ? 0xFFFF : vehicles.size();
    w.u16(static_cast<uint16_t>(nv));
    for (size_t k = 0; k < nv; ++k)
        writeVehicleState(w, vehicles[k]);
    const size_t nc = controlPoints.size() > 0xFFFF ? 0xFFFF : controlPoints.size();
    w.u16(static_cast<uint16_t>(nc));
    for (size_t k = 0; k < nc; ++k)
        writeControlPointState(w, controlPoints[k]);
    const size_t nr = projectiles.size() > 0xFFFF ? 0xFFFF : projectiles.size();
    w.u16(static_cast<uint16_t>(nr));
    for (size_t k = 0; k < nr; ++k)
        writeProjectileState(w, projectiles[k]);
    w.finish();
}

void encodePlayerStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<uint8_t>& ids,
                        const std::vector<PlayerState>& states)
{
    Writer w(out);
    w.begin(MsgPlayerStream);
    w.u32(tick);
    w.f64(time);
    size_t n = ids.size() < states.size() ? ids.size() : states.size();
    if (n > 255)
        n = 255;
    w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; ++k)
    {
        w.u8(ids[k]);
        writePlayerState(w, states[k]);
    }
    w.finish();
}

void encodeVehicleStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<VehicleState>& v)
{
    Writer w(out);
    w.begin(MsgVehicleStream);
    w.u32(tick);
    w.f64(time);
    const size_t n = v.size() > 0xFFFF ? 0xFFFF : v.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t k = 0; k < n; ++k)
        writeVehicleState(w, v[k]);
    w.finish();
}

void encodeProjectileStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<ProjectileState>& p)
{
    Writer w(out);
    w.begin(MsgProjectileStream);
    w.u32(tick);
    w.f64(time);
    const size_t n = p.size() > 0xFFFF ? 0xFFFF : p.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t k = 0; k < n; ++k)
        writeProjectileState(w, p[k]);
    w.finish();
}

void encodeGameStream(std::vector<uint8_t>& out, uint32_t tick, double time, const GameState& g)
{
    Writer w(out);
    w.begin(MsgGameStream);
    w.u32(tick);
    w.f64(time);
    writeGameState(w, g);
    w.finish();
}

void encodeAimStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<AimState>& a)
{
    Writer w(out);
    w.begin(MsgAimStream);
    w.u32(tick);
    w.f64(time);
    const size_t n = a.size() > 255 ? 255 : a.size();
    w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; ++k)
    {
        w.u8(a[k].player);
        w.u8(a[k].flags);
        writeVec3(w, a[k].cameraPos);
        writeVec3(w, a[k].cameraRot);
        writeVec3(w, a[k].weaponPos);
        writeVec3(w, a[k].weaponRot);
    }
    w.finish();
}

// ---------------------------------------------------------------- replies

void encodeOk(std::vector<uint8_t>& out, uint32_t requestId)
{
    Writer w(out);
    w.begin(MsgOk);
    w.u32(requestId);
    w.finish();
}

void encodeError(std::vector<uint8_t>& out, uint32_t requestId, uint16_t code, const std::string& message)
{
    proto::encodeError(out, requestId, code, message);
}

void encodeConsoleResult(std::vector<uint8_t>& out, uint32_t requestId, bool truncated,
                         const std::vector<std::string>& lines)
{
    Writer w(out);
    w.begin(MsgConsoleResult);
    w.u32(requestId);
    w.u8(truncated ? 1 : 0);
    const size_t n = lines.size() > 0xFFFF ? 0xFFFF : lines.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t k = 0; k < n; ++k)
        w.str(lines[k]);
    w.finish();
}

void encodeObjectList(std::vector<uint8_t>& out, uint32_t requestId, bool more, const std::vector<ObjectInfo>& objects)
{
    Writer w(out);
    w.begin(MsgObjectList);
    w.u32(requestId);
    w.u8(more ? 1 : 0);
    const size_t n = objects.size() > 0xFFFF ? 0xFFFF : objects.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t k = 0; k < n; ++k)
    {
        const ObjectInfo& o = objects[k];
        w.u32(o.objectId);
        w.u32(o.netId);
        w.str(o.templateName);
        writeVec3(w, o.pos);
        writeVec3(w, o.rot);
        w.f32(o.health);
        w.u8(o.flags);
    }
    w.finish();
}

void encodeObjectSpawned(std::vector<uint8_t>& out, uint32_t requestId, uint32_t objectId, uint32_t netId)
{
    Writer w(out);
    w.begin(MsgObjectSpawned);
    w.u32(requestId);
    w.u32(objectId);
    w.u32(netId);
    w.finish();
}

void encodeScriptList(std::vector<uint8_t>& out, uint32_t requestId,
                      const std::vector<std::pair<uint32_t, std::string> >& scripts)
{
    Writer w(out);
    w.begin(MsgScriptList);
    w.u32(requestId);
    const size_t n = scripts.size() > 255 ? 255 : scripts.size();
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i)
    {
        w.u32(scripts[i].first);
        w.str(scripts[i].second);
    }
    w.finish();
}

void encodePing(std::vector<uint8_t>& out, uint16_t type, uint32_t requestId, uint64_t clock)
{
    Writer w(out);
    w.begin(type);
    w.u32(requestId);
    w.u64(clock);
    w.finish();
}

// ---------------------------------------------------------------- events

void beginEvent(Writer& w, uint16_t type, uint32_t seq, uint32_t tick)
{
    w.begin(type);
    w.u32(seq);
    w.u32(tick);
}

void encodeGameStateEvent(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const GameState& g, uint8_t previous)
{
    Writer w(out);
    beginEvent(w, EvGameState, seq, tick);
    writeGameState(w, g);
    w.u8(previous);
    w.finish();
}

void encodeRoundStart(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const std::string& map,
                      const std::string& mode, const int32_t tickets[2])
{
    Writer w(out);
    beginEvent(w, EvRoundStart, seq, tick);
    w.str(map);
    w.str(mode);
    w.i32(tickets[0]);
    w.i32(tickets[1]);
    w.finish();
}

void encodeRoundEnd(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const int32_t tickets[2], uint8_t winner,
                    uint8_t victoryType)
{
    Writer w(out);
    beginEvent(w, EvRoundEnd, seq, tick);
    w.i32(tickets[0]);
    w.i32(tickets[1]);
    w.u8(winner);
    w.u8(victoryType);
    w.finish();
}

void encodePlayerConnect(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const PlayerInfo& p)
{
    Writer w(out);
    beginEvent(w, EvPlayerConnect, seq, tick);
    writePlayerInfo(w, p);
    w.finish();
}

void encodePlayerDisconnect(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t reason,
                            uint16_t engineReason)
{
    Writer w(out);
    beginEvent(w, EvPlayerDisconnect, seq, tick);
    w.u8(player);
    w.u8(reason);
    w.u16(engineReason);
    w.finish();
}

void encodeSpawn(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t team, uint32_t rootNetId,
                 uint32_t rootObjectId, const std::string& rootTemplate, const float pos[3], const float rot[3])
{
    Writer w(out);
    beginEvent(w, EvSpawn, seq, tick);
    w.u8(player);
    w.u8(team);
    w.u32(rootNetId);
    w.u32(rootObjectId);
    w.str(rootTemplate);
    writeVec3(w, pos);
    writeVec3(w, rot);
    w.finish();
}

void encodeDeath(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t team, uint32_t netId,
                 const std::string& templateName, const float pos[3])
{
    Writer w(out);
    beginEvent(w, EvDeath, seq, tick);
    w.u8(player);
    w.u8(team);
    w.u32(netId);
    w.str(templateName);
    writeVec3(w, pos);
    w.finish();
}

void encodeRevive(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t revived, int16_t medic)
{
    Writer w(out);
    beginEvent(w, EvRevive, seq, tick);
    w.u8(revived);
    w.i16(medic);
    w.finish();
}

void encodeTeamChange(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t oldTeam,
                      uint8_t newTeam, uint8_t flags)
{
    Writer w(out);
    beginEvent(w, EvTeamChange, seq, tick);
    w.u8(player);
    w.u8(oldTeam);
    w.u8(newTeam);
    w.u8(flags);
    w.finish();
}

void encodeScoreChange(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const PlayerScore& s)
{
    Writer w(out);
    beginEvent(w, EvScoreChange, seq, tick);
    w.u8(player);
    writePlayerScore(w, s);
    w.finish();
}

void encodeKill(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const KillEvent& k)
{
    Writer w(out);
    beginEvent(w, EvKill, seq, tick);
    w.u8(k.victim);
    w.i16(k.attacker);
    w.u8(k.kind);
    w.str(k.weapon);
    w.u8(k.victimTeam);
    w.u32(k.victimRootNetId);
    w.str(k.victimRootTemplate);
    writeVec3(w, k.victimPos);
    w.u8(k.attackerTeam);
    w.u32(k.attackerVehicleNetId);
    w.str(k.attackerVehicleTemplate);
    writeVec3(w, k.attackerPos);
    const size_t n = k.assists.size() > 255 ? 255 : k.assists.size();
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i)
    {
        w.u8(k.assists[i].first);
        w.i32(k.assists[i].second);
    }
    w.finish();
}

void encodeDamage(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const DamageEvent& d)
{
    Writer w(out);
    beginEvent(w, EvDamage, seq, tick);
    w.i16(d.victimPlayer);
    w.u32(d.victimRootNetId);
    w.u32(d.victimRootObjectId);
    w.str(d.victimTemplate);
    w.i16(d.attacker);
    w.str(d.weapon);
    w.str(d.projectile);
    w.f32(d.amount);
    w.f32(d.before);
    w.f32(d.after);
    w.f32(d.max);
    writeVec3(w, d.pos);
    w.u8(d.flags);
    w.finish();
}

void encodeVehicleSeat(std::vector<uint8_t>& out, uint16_t type, uint32_t seq, uint32_t tick, const VehicleSeatEvent& e)
{
    Writer w(out);
    beginEvent(w, type, seq, tick);
    w.u8(e.player);
    w.u8(e.seat);
    w.u8(e.flags);
    w.u32(e.rootNetId);
    w.u32(e.rootObjectId);
    w.str(e.rootTemplate);
    w.str(e.seatTemplate);
    w.finish();
}

void encodeVehicleDestroyed(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t netId, uint32_t objectId,
                            const std::string& templateName, int16_t attacker, const float pos[3])
{
    Writer w(out);
    beginEvent(w, EvVehicleDestroyed, seq, tick);
    w.u32(netId);
    w.u32(objectId);
    w.str(templateName);
    w.i16(attacker);
    writeVec3(w, pos);
    w.finish();
}

void encodeControlPoint(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t kind, const ControlPointState& c)
{
    Writer w(out);
    beginEvent(w, EvControlPoint, seq, tick);
    w.u8(kind);
    writeControlPointState(w, c);
    w.finish();
}

void encodeChat(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ChatEvent& c)
{
    Writer w(out);
    beginEvent(w, EvChat, seq, tick);
    w.i16(c.sender);
    w.u8(c.team);
    w.u8(c.channel);
    w.str(c.channelName);
    w.u32(c.engineFlags);
    w.u8(c.textFlags);
    w.str(c.text);
    w.finish();
}

void encodeProjectileFired(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileState& p)
{
    Writer w(out);
    beginEvent(w, EvProjectileFired, seq, tick);
    writeProjectileState(w, p);
    w.finish();
}

void encodeProjectileHit(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileHitEvent& h)
{
    Writer w(out);
    beginEvent(w, EvProjectileHit, seq, tick);
    w.u32(h.id);
    w.str(h.templateName);
    w.i16(h.owner);
    w.u8(h.flags);
    w.u32(h.firstVictimNetId);
    writeVec3(w, h.pos);
    w.finish();
}

void encodeKick(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const std::string& name)
{
    Writer w(out);
    beginEvent(w, EvKick, seq, tick);
    w.u8(player);
    w.str(name);
    w.finish();
}

void encodeBan(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const std::string& name,
               uint8_t banType, uint32_t expiry)
{
    Writer w(out);
    beginEvent(w, EvBan, seq, tick);
    w.u8(player);
    w.str(name);
    w.u8(banType);
    w.u32(expiry);
    w.finish();
}

void encodeBot(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t kind, uint8_t team,
               uint32_t connection)
{
    Writer w(out);
    beginEvent(w, EvBot, seq, tick);
    w.u8(player);
    w.u8(kind);
    w.u8(team);
    w.u32(connection);
    w.finish();
}

void encodeWeaponFire(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const WeaponFireEvent& e)
{
    Writer w(out);
    beginEvent(w, EvWeaponFire, seq, tick);
    w.i16(e.shooter);
    w.u32(e.rootNetId);
    w.str(e.weapon);
    w.str(e.projectile);
    w.u8(e.rounds);
    writeVec3(w, e.origin);
    writeVec3(w, e.velocity);
    w.finish();
}

void encodeWeapons(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player,
                   const std::vector<WeaponSlot>& slots)
{
    Writer w(out);
    beginEvent(w, EvWeapons, seq, tick);
    w.u8(player);
    const size_t n = slots.size() > kMaxWeaponSlots ? kMaxWeaponSlots : slots.size();
    w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; ++k)
    {
        w.u8(slots[k].slot);
        w.str(slots[k].templateName);
    }
    w.finish();
}

void encodeVoteResult(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t requestId,
                      const std::string& text, uint8_t outcome, uint16_t yes, uint16_t no, uint16_t eligible)
{
    Writer w(out);
    beginEvent(w, EvVoteResult, seq, tick);
    w.u32(requestId);
    w.str(text);
    w.u8(outcome);
    w.u16(yes);
    w.u16(no);
    w.u16(eligible);
    w.finish();
}

void encodeScriptFinished(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t instance,
                          const std::string& name, uint8_t reason, const std::string& result, uint32_t calls,
                          uint32_t totalUs, uint32_t maxUs)
{
    Writer w(out);
    beginEvent(w, EvScriptFinished, seq, tick);
    w.u32(instance);
    w.str(name);
    w.u8(reason);
    w.str(result);
    w.u32(calls);
    w.u32(totalUs);
    w.u32(maxUs);
    w.finish();
}

void encodeServerHealth(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const health::Window& h)
{
    Writer w(out);
    beginEvent(w, EvServerHealth, seq, tick);
    w.u16(h.ticks);
    w.u32(h.wallUs);
    w.u8(h.flags);
    w.u32(h.intervalP50Us);
    w.u32(h.intervalP99Us);
    w.u32(h.intervalMaxUs);
    w.u16(h.lateTicks);
    w.u16(h.shortTicks);
    w.u32(h.cpuP50Us);
    w.u32(h.cpuP99Us);
    w.u32(h.cpuMaxUs);
    w.u32(h.handlerP99Us);
    w.u32(h.handlerMaxUs);
    w.u32(h.involuntarySwitches);
    w.u32(h.majorFaults);
    w.u16(h.cpus);
    w.u16(h.hostBusy);
    w.u16(h.hostSteal);
    w.u16(h.hostIoWait);
    w.u32(h.residentKb);
    w.finish();
}

void encodeScriptEvent(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t instance, uint8_t kind,
                       const std::string& payload)
{
    Writer w(out);
    beginEvent(w, EvScriptEvent, seq, tick);
    w.u32(instance);
    w.u8(kind);
    w.str(payload.size() > kMaxScriptPayload ? payload.substr(0, kMaxScriptPayload) : payload);
    w.finish();
}

void encodeProjectileCollision(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileCollisionEvent& e)
{
    Writer w(out);
    beginEvent(w, EvProjectileCollision, seq, tick);
    w.u32(e.id);
    w.str(e.templateName);
    w.i16(e.shooter);
    w.u8(e.kind);
    w.u8(e.flags);
    w.u32(e.rootNetId);
    w.u32(e.rootObjectId);
    w.str(e.rootTemplate);
    w.str(e.partTemplate);
    w.i16(e.partPlayer);
    writeVec3(w, e.pos);
    writeVec3(w, e.normal);
    writeVec3(w, e.speed);
    w.u16(e.material);
    w.f32(e.latency);
    writeVec3(w, e.partPos);
    writeVec3(w, e.partRot);
    writeVec3(w, e.rootPos);
    writeVec3(w, e.rootRot);
    w.finish();
}

void encodeProjectileTrace(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileTraceEvent& e)
{
    Writer w(out);
    beginEvent(w, EvProjectileTrace, seq, tick);
    w.u32(e.id);
    w.str(e.templateName);
    w.i16(e.shooter);
    w.u32(e.firstTick);
    w.u8(e.flags);
    const size_t t = e.targets.size() > kMaxTraceTargets ? kMaxTraceTargets : e.targets.size();
    w.u8(static_cast<uint8_t>(t));
    for (size_t k = 0; k < t; ++k)
    {
        w.u32(e.targets[k].netId);
        w.str(e.targets[k].templateName);
    }
    size_t n = e.samples.size() > kMaxTraceSamples ? kMaxTraceSamples : e.samples.size();
    if (e.targets.size() && e.poses.size() / e.targets.size() < n)
        n = e.poses.size() / e.targets.size();
    w.u16(static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i)
    {
        const TraceSample& s = e.samples[i];
        writeVec3(w, s.pos);
        writeVec3(w, s.vel);
        w.f32(s.lag);
        for (size_t k = 0; k < t; ++k)
        {
            const TracePose& q = e.poses[i * e.targets.size() + k];
            writeVec3(w, q.pos);
            writeVec3(w, q.rot);
            writeVec3(w, q.testedPos);
            writeVec3(w, q.testedRot);
        }
    }
    w.finish();
}

// ---------------------------------------------------------------- inbound

bool isWebAdminMessage(uint16_t type)
{
    switch (type)
    {
    case MsgChallenge: case MsgRequestSnapshot: case MsgSubscribe: case MsgUnsubscribe:
    case CmdConsole: case CmdSay: case CmdTeleport: case CmdSetHealth: case CmdKillPlayer: case CmdSetTeam:
    case CmdSetScore: case CmdRank: case CmdMedal: case CmdGameEvent: case CmdHudEvent: case CmdKick: case CmdBan:
    case CmdMap: case CmdListObjects: case CmdSetObjectActive: case CmdSpawnObject: case CmdDeleteObject:
    case CmdStartVote: case CmdCancelVote: case CmdHudEventRaw: case CmdRoundClock: case CmdSquadOrder:
    case CmdStartScript: case CmdStopScript: case CmdScriptMessage: case CmdListScripts: case CmdSeatPreference:
    case CmdSetCollision:
    case MsgPing: case MsgPong: case MsgError:
        return true;
    default:
        return false;
    }
}

bool parseCommand(uint16_t type, const uint8_t* p, size_t n, Command& out, std::string* badValue)
{
    if (badValue)
        badValue->clear();
    out.type = type;
    Reader r(p, n);
    std::string raw, raw2;
    switch (type)
    {
    case MsgRequestSnapshot:
        out.requestId = r.u32();
        return r.done();

    case MsgPing:
    case MsgPong:
        out.requestId = r.u32();
        out.clock = r.u64();
        return r.done();

    case MsgError:
        out.requestId = r.u32();
        out.index = r.u16();   // the code
        r.str(raw);
        if (!r.done())
            return false;
        out.text = raw;
        return true;

    case MsgSubscribe:
    {
        out.requestId = r.u32();
        out.target = r.u8();
        out.value8 = r.u8();
        const uint8_t count = r.u8();
        for (uint8_t i = 0; i < count && r.ok(); ++i)
        {
            std::string f;
            r.str(f);
            out.filters.push_back(f);
        }
        if (!r.done())
            return false;
        const uint8_t s = out.target;
        const bool eventStream = s == StreamDamageEvents || s == StreamProjectileEvents ||
                                 s == StreamWeaponFireEvents || s == StreamWeaponEvents || s == StreamProjectileReplay ||
                                 s == StreamServerHealth;
        if (s != StreamPlayers && s != StreamVehicles && s != StreamProjectiles && s != StreamGame && s != StreamAim &&
            !eventStream)
            return bad(badValue, "unknown stream");
        if (!eventStream && (out.value8 < 1 || out.value8 > 30))
            return bad(badValue, "rate must be 1..30 Hz");
        const bool needsFilters = s == StreamProjectiles || s == StreamProjectileEvents || s == StreamProjectileReplay;
        const bool anyFilters = s == StreamWeaponFireEvents;
        if (!anyFilters && (needsFilters ? out.filters.empty() : !out.filters.empty()))
            return bad(badValue, needsFilters ? "this stream needs at least one template filter"
                                              : "this stream takes no filters");
        if (out.filters.size() > kMaxFilters)
            return bad(badValue, "at most 64 filters");
        for (size_t i = 0; i < out.filters.size(); ++i)
        {
            std::string converted;
            if (!engineText(out.filters[i], kMaxTemplateName, false, converted, badValue, "bad template filter"))
                return false;
            out.filters[i] = converted;
        }
        return true;
    }

    case MsgUnsubscribe:
        out.requestId = r.u32();
        out.target = r.u8();
        return r.done();

    case CmdConsole:
        out.requestId = r.u32();
        r.str(raw);
        if (!r.done())
            return false;
        return engineText(raw, kMaxConsoleLine, false, out.text, badValue,
                          "the console line must be 1..4096 bytes of UTF-8 without control characters");

    case CmdSay:
        out.requestId = r.u32();
        out.target = r.u8();
        out.player = r.u8();
        r.str(raw);
        if (!r.done())
            return false;
        if (out.target > 2)
            return bad(badValue, "target must be 0 (all), 1 (team) or 2 (player)");
        if (out.target == 1 && out.player != 1 && out.player != 2)
            return bad(badValue, "team must be 1 or 2");
        return engineText(raw, kMaxSayText, false, out.text, badValue,
                          "the text must be 1..1024 bytes of UTF-8 without control characters");

    case CmdTeleport:
        out.requestId = r.u32();
        readRef(r, out.ref);
        out.flags = r.u8();
        readVec3(r, out.pos);
        readVec3(r, out.rot);
        if (!r.done())
            return false;
        if (out.ref.kind < RefPlayer || out.ref.kind > RefConsoleId)
            return bad(badValue, "unknown object reference kind");
        if ((out.flags & 3) == 0 || (out.flags & ~3))
            return bad(badValue, "flags must set position, rotation or both");
        if ((out.flags & 1) && (!finite3(out.pos) || !inWorld(out.pos)))
            return bad(badValue, "position is not finite or outside the world");
        if ((out.flags & 2) && !finite3(out.rot))
            return bad(badValue, "rotation is not finite");
        return true;

    case CmdSetHealth:
        out.requestId = r.u32();
        readRef(r, out.ref);
        out.flags = r.u8();
        out.value = r.f32();
        if (!r.done())
            return false;
        if (out.ref.kind < RefPlayer || out.ref.kind > RefConsoleId)
            return bad(badValue, "unknown object reference kind");
        if (out.flags & ~1)
            return bad(badValue, "unknown flags");
        if (!std::isfinite(out.value) || out.value < 0.0f || ((out.flags & 1) ? out.value > 1.0f : out.value > 1.0e6f))
            return bad(badValue, "value must be 0..1 as a fraction or 0..1e6 hit points");
        return true;

    case CmdKillPlayer:
        out.requestId = r.u32();
        out.player = r.u8();
        out.flags = r.u8();
        if (!r.done())
            return false;
        return (out.flags & ~1) ? bad(badValue, "unknown flags") : true;

    case CmdSetTeam:
        out.requestId = r.u32();
        out.player = r.u8();
        out.value8 = r.u8();
        out.flags = r.u8();
        if (!r.done())
            return false;
        if (out.value8 != 1 && out.value8 != 2)
            return bad(badValue, "team must be 1 or 2");
        return (out.flags & ~1) ? bad(badValue, "unknown flags") : true;

    case CmdSetScore:
        out.requestId = r.u32();
        out.player = r.u8();
        out.flags = r.u8();
        for (int i = 0; i < 5; ++i)
            out.ints[i] = r.i32();
        if (!r.done())
            return false;
        if (out.flags == 0 || (out.flags & ~0x1F))
            return bad(badValue, "mask must select at least one of the five fields");
        return true;

    case CmdRank:
        out.requestId = r.u32();
        out.player = r.u8();
        out.value8 = r.u8();
        out.flags = r.u8();
        if (!r.done())
            return false;
        if (out.value8 > 21)
            return bad(badValue, "rank must be 0..21");
        return (out.flags & ~1) ? bad(badValue, "unknown flags") : true;

    case CmdMedal:
        out.requestId = r.u32();
        out.player = r.u8();
        out.ints[0] = r.i32();
        out.ints[1] = r.i32();
        return r.done();

    case CmdGameEvent:
    case CmdHudEvent:
        out.requestId = r.u32();
        out.player = r.u8();
        out.value8 = r.u8();
        out.ints[0] = r.i32();
        if (!r.done())
            return false;
        if (out.value8 > (type == CmdGameEvent ? 24 : 78))
            return bad(badValue, type == CmdGameEvent ? "event must be 0..24" : "event must be 0..78");
        return true;

    case CmdKick:
        out.requestId = r.u32();
        out.player = r.u8();
        r.str(raw);
        if (!r.done())
            return false;
        return engineText(raw, kMaxReason, true, out.text, badValue,
                          "the reason must be at most 256 bytes of UTF-8 without control characters");

    case CmdBan:
        out.requestId = r.u32();
        out.player = r.u8();
        out.target = r.u8();
        out.value8 = r.u8();
        out.seconds = r.u32();
        r.str(raw);
        if (!r.done())
            return false;
        if (out.target > 1)
            return bad(badValue, "by must be 0 (address) or 1 (CD key)");
        if (out.value8 > 2)
            return bad(badValue, "duration must be 0 (permanent), 1 (round) or 2 (seconds)");
        if (out.value8 == 2 && (out.seconds == 0 || out.seconds > 0x7FFFFFFFu))
            return bad(badValue, "seconds must be 1..2147483647");
        return engineText(raw, kMaxReason, true, out.text, badValue,
                          "the reason must be at most 256 bytes of UTF-8 without control characters");

    case CmdMap:
        out.requestId = r.u32();
        out.target = r.u8();
        out.index = r.u16();
        r.str(raw);
        r.str(raw2);
        out.size = r.u16();
        if (!r.done())
            return false;
        if (out.target < 1 || out.target > 4)
            return bad(badValue, "action must be 1..4");
        if (out.target == 4 && (raw.empty() || raw2.empty()))
            return bad(badValue, "action 4 needs a map name and a game mode");
        if (!engineText(raw, kMaxTemplateName, true, out.text, badValue, "bad map name"))
            return false;
        return engineText(raw2, kMaxTemplateName, true, out.text2, badValue, "bad game mode");

    case CmdListObjects:
        out.requestId = r.u32();
        r.str(raw);
        out.maxResults = r.u16();
        if (!r.done())
            return false;
        if (out.maxResults < 1 || out.maxResults > kMaxListObjects)
            return bad(badValue, "maximum entries must be 1..4096");
        return engineText(raw, kMaxTemplateName, false, out.text, badValue, "bad template name");

    case CmdSetObjectActive:
    case CmdDeleteObject:
        out.requestId = r.u32();
        readRef(r, out.ref);
        if (!r.done())
            return false;
        if (out.ref.kind != RefNetId && out.ref.kind != RefConsoleId)
            return bad(badValue, "the object reference must be a networkable id (2) or a console id (3)");
        return true;

    case CmdSpawnObject:
        out.requestId = r.u32();
        r.str(raw);
        readVec3(r, out.pos);
        readVec3(r, out.rot);
        out.value8 = r.u8();
        if (!r.done())
            return false;
        if (!finite3(out.pos) || !inWorld(out.pos) || !finite3(out.rot))
            return bad(badValue, "position or rotation is not finite or outside the world");
        if (out.value8 > 3)
            return bad(badValue, "team must be 0..3");
        return engineText(raw, kMaxTemplateName, false, out.text, badValue, "bad template name");

    case CmdStartVote:
        out.requestId = r.u32();
        r.str(raw);
        out.seconds = r.u16();
        out.target = r.u8();
        out.value8 = r.u8();
        out.flags = r.u8();
        if (!r.done())
            return false;
        if (out.seconds < static_cast<uint32_t>(vote::kMinSeconds) || out.seconds > static_cast<uint32_t>(vote::kMaxSeconds))
            return bad(badValue, "seconds must be 5..600");
        if (out.target > 2)
            return bad(badValue, "team must be 0 (everyone), 1 or 2");
        if (out.value8 < 1 || out.value8 > 100)
            return bad(badValue, "percent must be 1..100");
        if (out.flags > 64)
            return bad(badValue, "minimum ballots must be 0..64");
        return engineText(raw, vote::kMaxText, false, out.text, badValue,
                          "the question must be 1..96 bytes of UTF-8 without control characters");

    case CmdCancelVote:
    case CmdListScripts:
        out.requestId = r.u32();
        return r.done();

    case CmdStartScript:
    {
        out.requestId = r.u32();
        out.instance = r.u32();
        out.value8 = r.u8();
        out.flags = r.u8();
        r.str(out.text);
        r.str(out.text2);
        r.str(out.source);
        if (!r.done())
            return false;
        std::string unused;
        if (out.instance == 0)
            return bad(badValue, "the instance must not be 0");
        if (out.value8 == 0)
            return bad(badValue, "the script API version must be 1 or more");
        if (out.flags != 0)
            return bad(badValue, "flags must be 0");
        if (out.text.empty() || out.text.size() > kMaxScriptName)
            return bad(badValue, "the name must be 1..64 printable ASCII bytes");
        for (size_t i = 0; i < out.text.size(); ++i)
            if (static_cast<unsigned char>(out.text[i]) < 0x20 || static_cast<unsigned char>(out.text[i]) > 0x7E)
                return bad(badValue, "the name must be 1..64 printable ASCII bytes");
        if (!utf8ToCp1252(out.text2, unused))
            return bad(badValue, "the arguments are not UTF-8");
        if (out.source.empty() || out.source.size() > kMaxScriptSource)
            return bad(badValue, "the source must be 1..60000 bytes");
        if (!utf8ToCp1252(out.source, unused))
            return bad(badValue, "the source is not UTF-8");
        return true;
    }

    case CmdStopScript:
        out.requestId = r.u32();
        out.instance = r.u32();
        return r.done();

    case CmdSeatPreference:
        out.requestId = r.u32();
        out.player = r.u8();
        out.value8 = r.u8();
        if (!r.done())
            return false;
        return seat::validPreference(out.value8) ? true
                                                 : bad(badValue, "seat must be 0 (first), 1 (second) or 255 (none)");

    case CmdSetCollision:
        out.requestId = r.u32();
        readRef(r, out.ref);
        out.flags = r.u8();
        if (!r.done())
            return false;
        if (out.ref.kind < RefPlayer || out.ref.kind > RefConsoleId)
            return bad(badValue, "unknown object reference kind");
        if (out.flags & ~1)
            return bad(badValue, "unknown flags (bit 0 is collision on)");
        return true;

    case CmdScriptMessage:
    {
        out.requestId = r.u32();
        out.instance = r.u32();
        r.str(out.text2);
        if (!r.done())
            return false;
        std::string unused;
        if (out.text2.size() > kMaxScriptPayload)
            return bad(badValue, "the message is at most 8192 bytes");
        if (!utf8ToCp1252(out.text2, unused))
            return bad(badValue, "the message is not UTF-8");
        return true;
    }

    case CmdHudEventRaw:
    {
        out.requestId = r.u32();
        out.target16 = r.i16();
        out.value8 = r.u8();
        const uint8_t length = r.u8();
        for (uint8_t i = 0; i < length && r.ok(); ++i)
            out.bytes.push_back(r.u8());
        if (!r.done())
            return false;
        if (out.target16 < -1 || out.target16 > 255)
            return bad(badValue, "player must be 0..255 or -1 (every client)");
        if (!hudEventRawAllowed(out.value8))
            return bad(badValue, "HUD event not allowed: 40, 41, 42, 43, 44, 51, 64, 65, 66, 70, 71, 74 or 83");
        if (out.bytes.size() > kMaxHudPayload)
            return bad(badValue, "the payload is at most 52 bytes");
        return true;
    }

    case CmdRoundClock:
        out.requestId = r.u32();
        out.target16 = r.i16();
        out.value = r.f32();
        if (!r.done())
            return false;
        if (out.target16 < -1 || out.target16 > 255)
            return bad(badValue, "player must be 0..255 or -1 (every client)");
        if (!(out.value >= 0.0f) || out.value > kMaxRoundClockSeconds)
            return bad(badValue, "seconds must be 0..86400");
        return true;

    case CmdSquadOrder:
        out.requestId = r.u32();
        out.target = r.u8();
        out.player = r.u8();
        out.value8 = r.u8();
        out.flags = r.u8();
        out.netId = r.u32();
        for (int k = 0; k < 3; ++k)
            readVec3(r, out.points + 3 * k);
        if (!r.done())
            return false;
        if (out.target != 1 && out.target != 2)
            return bad(badValue, "team must be 1 or 2");
        if (out.player == 0)
            return bad(badValue, "squad must be 1..255");
        if (out.flags & ~1)
            return bad(badValue, "unknown flags");
        if (out.netId > 0xFFFF)
            return bad(badValue, "the target networkable id must be 0..65535");
        for (int k = 0; k < 3; ++k)
            if (!finite3(out.points + 3 * k))
                return bad(badValue, "a point is not finite");
        return true;

    default:
        return false;
    }
}

// ---------------------------------------------------------------- text

std::string cp1252ToUtf8(const std::string& in, bool keepTab)
{
    std::string out;
    out.reserve(in.size() + in.size() / 4);
    for (size_t i = 0; i < in.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 || c == 0x7F)
            out += (keepTab && c == '\t') ? '\t' : '?';
        else if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0xA0)
            appendUtf8(out, kCp1252High[c - 0x80] ? kCp1252High[c - 0x80] : c);
        else
            appendUtf8(out, c);
    }
    return out;
}

bool utf8ToCp1252(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size())
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else return false;
        if (i + len > in.size())
            return false;
        for (size_t k = 1; k < len; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        static const uint32_t kMin[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;

        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF))
        {
            out += static_cast<char>(cp);
            continue;
        }
        char mapped = '?';
        for (int k = 0; k < 32; ++k)
        {
            if ((kCp1252High[k] && kCp1252High[k] == cp) || (!kCp1252High[k] && cp == 0x80u + k))
            {
                mapped = static_cast<char>(0x80 + k);
                break;
            }
        }
        out += mapped;
    }
    return true;
}

bool hasControlBytes(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F)
            return true;
    }
    return false;
}

bool parseHex(const std::string& hex, std::vector<uint8_t>& out)
{
    out.clear();
    if (hex.size() % 2)
        return false;
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        int v = 0;
        for (int k = 0; k < 2; ++k)
        {
            const char c = hex[i + k];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = v * 16 + d;
        }
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

std::string toHex(const uint8_t* data, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 15];
    }
    return s;
}

} // namespace admin
} // namespace serverext
