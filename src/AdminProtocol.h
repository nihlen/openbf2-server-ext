// Wire format of the admin channel, protocol version 1 (docs/findings/admin-phase1.md; design in
// docs/admin-channel-draft.md). Engine-free: the admin I/O thread, the main thread and the unit
// tests all use it. Framing, Writer and Reader are the bot channel's (Protocol.h).

#ifndef SERVEREXT_ADMINPROTOCOL_H
#define SERVEREXT_ADMINPROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Protocol.h"

namespace serverext {
namespace admin {

const uint16_t kVersion = 1;
const uint32_t kMaxInboundPayload = 64 * 1024;      // WebAdmin -> module
const uint32_t kMaxOutboundPayload = 1024 * 1024;   // module -> WebAdmin
const size_t kNonceSize = 32;
const size_t kProofSize = 32;
const size_t kMaxServerId = 64;
const size_t kMaxConsoleLine = 4096;
const size_t kMaxConsoleOutput = 256 * 1024;
const uint16_t kMaxListObjects = 4096;
const uint32_t kNoObjectId = 0xFFFFFFFFu;
const uint16_t kNoIndex = 0xFFFF;
const size_t kMaxHudPayload = 52;   // PostRemoteEvent's payload (R7)
const float kMaxRoundClockSeconds = 86400.0f;
const size_t kMaxScriptName = 64;        // StartScript name (Script.h has the same limits)
const size_t kMaxScriptSource = 60000;   // StartScript source
const size_t kMaxScriptPayload = 8192;   // ScriptMessage message, ScriptEvent payload

// R7: the HUD event ids HudEventRaw accepts, the engine's sendHudEventRaw whitelist: their retail
// client handlers read the payload as plain data.
bool hudEventRawAllowed(uint8_t id);

enum Type : uint16_t
{
    // session
    MsgHello = 0x1001, MsgChallenge = 0x1002, MsgAuth = 0x1003, MsgWelcome = 0x1004,
    // state
    MsgSnapshot = 0x1101, MsgRequestSnapshot = 0x1102, MsgSubscribe = 0x1103, MsgUnsubscribe = 0x1104,
    MsgPlayerStream = 0x1111, MsgVehicleStream = 0x1112, MsgProjectileStream = 0x1113, MsgGameStream = 0x1114,
    MsgAimStream = 0x1115,
    // events
    EvGameState = 0x1201, EvRoundStart = 0x1202, EvRoundEnd = 0x1203, EvPlayerConnect = 0x1204,
    EvPlayerDisconnect = 0x1205, EvSpawn = 0x1206, EvDeath = 0x1207, EvRevive = 0x1208, EvTeamChange = 0x1209,
    EvScoreChange = 0x120A, EvKill = 0x120B, EvDamage = 0x120C, EvEnterVehicle = 0x120D, EvExitVehicle = 0x120E,
    EvVehicleDestroyed = 0x120F, EvControlPoint = 0x1210, EvChat = 0x1211, EvProjectileFired = 0x1212,
    EvProjectileHit = 0x1213, EvKick = 0x1214, EvBan = 0x1215, EvBot = 0x1216,
    EvWeaponFire = 0x1217, EvWeapons = 0x1218, EvProjectileCollision = 0x1219, EvProjectileTrace = 0x121A,
    EvVoteResult = 0x121B, EvScriptFinished = 0x121C, EvScriptEvent = 0x121D,
    // commands
    CmdConsole = 0x1301, CmdSay = 0x1302, CmdTeleport = 0x1303, CmdSetHealth = 0x1304, CmdKillPlayer = 0x1305,
    CmdSetTeam = 0x1306, CmdSetScore = 0x1307, CmdRank = 0x1308, CmdMedal = 0x1309, CmdGameEvent = 0x130A,
    CmdHudEvent = 0x130B, CmdKick = 0x130C, CmdBan = 0x130D, CmdMap = 0x130E, CmdListObjects = 0x130F,
    CmdSetObjectActive = 0x1310, CmdSpawnObject = 0x1311, CmdDeleteObject = 0x1312, CmdStartVote = 0x1313,
    CmdCancelVote = 0x1314, CmdHudEventRaw = 0x1315, CmdRoundClock = 0x1316, CmdSquadOrder = 0x1317,
    CmdStartScript = 0x1318, CmdStopScript = 0x1319, CmdScriptMessage = 0x131A, CmdListScripts = 0x131B,
    CmdSeatPreference = 0x131C, CmdSetCollision = 0x131D,
    // replies
    MsgOk = 0x1401, MsgConsoleResult = 0x1402, MsgObjectList = 0x1403, MsgObjectSpawned = 0x1404,
    MsgScriptList = 0x1405,
    // keepalive, either direction
    MsgPing = 0x1501, MsgPong = 0x1502,
    MsgError = 0xFFFF
};

enum ErrorCode : uint16_t
{
    ErrVersion = 1, ErrMalformed = 2, ErrUnknownType = 3, ErrUnknownPlayer = 4, ErrUnavailable = 5,
    ErrRefused = 6, ErrInvalidValue = 7, ErrAuth = 8, ErrDisabled = 9
};

// Welcome capability bits.
const uint8_t kCapConsole = 1;
const uint8_t kCapObjects = 2;
const uint8_t kCapIdentity = 4;
const uint8_t kCapScripts = 8;   // the script commands (docs/admin-channel-draft.md "Scripts")
const uint8_t kCapSeatPreference = 16;   // SeatPreference (docs/admin-channel-draft.md "Seat preferences")
const uint8_t kCapSetCollision = 32;     // SetCollision (docs/admin-channel-draft.md "Command semantics")

// Subscribe streams.
enum Stream : uint8_t
{
    StreamPlayers = 1, StreamVehicles = 2, StreamProjectiles = 3, StreamGame = 4, StreamAim = 5,
    StreamDamageEvents = 16, StreamProjectileEvents = 17, StreamWeaponFireEvents = 18, StreamWeaponEvents = 19,
    StreamProjectileReplay = 20
};

// ObjectRef kinds.
enum RefKind : uint8_t { RefPlayer = 1, RefNetId = 2, RefConsoleId = 3 };

// PlayerState life values and flags.
const uint8_t kLifeDead = 0, kLifeAlive = 1, kLifeManDown = 2;
const uint8_t kPlayerInVehicle = 1, kPlayerCommander = 2, kPlayerSquadLeader = 4;
// PlayerInfo flags.
const uint8_t kInfoAi = 1, kInfoBotChannel = 2, kInfoConnected = 4, kInfoIdentity = 8;
const uint8_t kSeatNone = 255;
const uint8_t kViewModeNone = 255;   // PlayerState view mode: dead or no camera

// Kill kinds.
enum KillKind : uint8_t { KillNormal = 1, KillTeam = 2, KillSuicide = 3, KillRoad = 4, KillNoAttacker = 5 };
// PlayerDisconnect reasons.
enum DisconnectReason : uint8_t
{
    DiscUnknown = 0, DiscLeft = 1, DiscKicked = 2, DiscTimedOut = 3, DiscBanned = 4, DiscOther = 5
};
// Bot event kinds.
enum BotKind : uint8_t { BotCreated = 1, BotRemoved = 2, BotTeamChange = 3 };

// ---------------------------------------------------------------- shared structures

struct GameState
{
    uint8_t status;        // 0 none
    uint8_t beginRound;    // 255 none
    float timeToNext;      // -1 none
    int32_t tickets[2];    // -1 unknown
    float roundTime;       // -1 not started
    float timeLimit;       // 0 none
    uint8_t winner;
    uint8_t victoryType;
    GameState() : status(0), beginRound(255), timeToNext(-1.0f), roundTime(-1.0f), timeLimit(0.0f), winner(0),
                  victoryType(0) { tickets[0] = tickets[1] = -1; }
};

struct PlayerState
{
    uint8_t team, squad, life, flags;
    uint16_t ping;
    uint32_t rootNetId;
    uint32_t rootObjectId;
    uint8_t seat;
    float pos[3], rot[3], vel[3];
    float health;
    uint8_t viewMode;    // camera view mode, kViewModeNone when dead or without a camera
    uint16_t lagMs;      // lag compensation for this player's projectiles, ms (IServerExtension::getLagCompensation)
    PlayerState();
};

struct PlayerScore
{
    int32_t score, teamScore, kills, deaths, teamKills;
    int16_t rank;
    PlayerScore() : score(0), teamScore(0), kills(0), deaths(0), teamKills(0), rank(-1) {}
    bool operator==(const PlayerScore& o) const
    {
        return score == o.score && teamScore == o.teamScore && kills == o.kills && deaths == o.deaths &&
               teamKills == o.teamKills && rank == o.rank;
    }
    bool operator!=(const PlayerScore& o) const { return !(*this == o); }
};

struct PlayerInfo
{
    uint8_t id;
    std::string name;
    uint8_t flags;
    int32_t profileId;
    std::string ip, hash;
    PlayerScore score;
    PlayerState state;
    PlayerInfo() : id(0), flags(0), profileId(0) {}
};

struct VehicleState
{
    uint32_t netId, objectId;
    std::string templateName;
    uint8_t team, flags;
    float pos[3], rot[3], vel[3];
    float health;
    uint8_t seats;
    uint32_t occupied;
    VehicleState();
};

struct ControlPointState
{
    uint32_t objectId, netId;
    std::string templateName;
    uint8_t team, flagTeam;
    float takeOver;
    uint8_t capturing;
    float pos[3];
    ControlPointState();
};

struct ProjectileState
{
    uint32_t id;
    std::string templateName;
    int16_t owner;
    uint8_t team, flags;
    uint32_t targetNetId;
    float pos[3], rot[3], vel[3];
    float timeToLive;
    ProjectileState();
};

// AimStream entry flags.
const uint8_t kAimCamera = 1, kAimWeapon = 2;

struct AimState
{
    uint8_t player, flags;
    float cameraPos[3], cameraRot[3];
    float weaponPos[3], weaponRot[3];
    AimState();
};

// The weapons in a player's active slots (Weapons event).
const size_t kMaxWeaponSlots = 3;
struct WeaponSlot
{
    uint8_t slot;
    std::string templateName;
    WeaponSlot() : slot(0) {}
};

struct MapEntry
{
    std::string name, mode;
    uint16_t size;
    MapEntry() : size(0) {}
};

struct WelcomeInfo
{
    uint32_t session, tick;
    float tickRate;
    std::string serverName;
    uint16_t gamePort, gameSpyPort;
    uint8_t maxPlayers, capabilities;
    std::string map, mode;
    uint16_t mapSize, mapIndex;
    std::vector<MapEntry> maps;
    std::string team1, team2;
    GameState game;
    WelcomeInfo() : session(0), tick(0), tickRate(30.0f), gamePort(0), gameSpyPort(0), maxPlayers(0),
                    capabilities(0), mapSize(0), mapIndex(kNoIndex) {}
};

struct ObjectRef
{
    uint8_t kind;
    uint32_t id;
    ObjectRef() : kind(0), id(0) {}
};

struct ObjectInfo
{
    uint32_t objectId, netId;
    std::string templateName;
    float pos[3], rot[3];
    float health;
    uint8_t flags;
    ObjectInfo();
};

// ---------------------------------------------------------------- structure writers and readers

void writeGameState(proto::Writer& w, const GameState& g);
void writePlayerState(proto::Writer& w, const PlayerState& s);
void writePlayerScore(proto::Writer& w, const PlayerScore& s);
void writePlayerInfo(proto::Writer& w, const PlayerInfo& p);
void writeVehicleState(proto::Writer& w, const VehicleState& v);
void writeControlPointState(proto::Writer& w, const ControlPointState& c);
void writeProjectileState(proto::Writer& w, const ProjectileState& p);
void writeVec3(proto::Writer& w, const float v[3]);

void readGameState(proto::Reader& r, GameState& g);
void readPlayerState(proto::Reader& r, PlayerState& s);
void readPlayerScore(proto::Reader& r, PlayerScore& s);
void readPlayerInfo(proto::Reader& r, PlayerInfo& p);
void readVehicleState(proto::Reader& r, VehicleState& v);
void readControlPointState(proto::Reader& r, ControlPointState& c);
void readProjectileState(proto::Reader& r, ProjectileState& p);
void readVec3(proto::Reader& r, float v[3]);

// ---------------------------------------------------------------- session

// The HMAC input for a proof (docs/findings/admin-phase1.md, Session).
std::vector<uint8_t> proofTranscript(const char* label, const std::string& serverId, const uint8_t moduleNonce[kNonceSize],
                                     const uint8_t webAdminNonce[kNonceSize]);
extern const char* const kWebAdminProofLabel;   // "OBF2-ADMIN-1 webadmin"
extern const char* const kServerProofLabel;     // "OBF2-ADMIN-1 server"

struct ChallengeMsg
{
    uint8_t nonce[kNonceSize];
    uint8_t proof[kProofSize];
};

void encodeHello(std::vector<uint8_t>& out, const std::string& serverId, const uint8_t nonce[kNonceSize],
                 const std::string& build);
bool parseChallenge(const uint8_t* p, size_t n, ChallengeMsg& out);
void encodeAuth(std::vector<uint8_t>& out, const uint8_t proof[kProofSize]);
void encodeWelcome(std::vector<uint8_t>& out, const WelcomeInfo& w);

// ---------------------------------------------------------------- state

void encodeSnapshot(std::vector<uint8_t>& out, uint32_t requestId, uint32_t lastSeq, uint32_t tick, double time,
                    const GameState& game, const std::vector<PlayerInfo>& players,
                    const std::vector<VehicleState>& vehicles, const std::vector<ControlPointState>& controlPoints,
                    const std::vector<ProjectileState>& projectiles);
void encodePlayerStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<uint8_t>& ids,
                        const std::vector<PlayerState>& states);
void encodeVehicleStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<VehicleState>& v);
void encodeProjectileStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<ProjectileState>& p);
void encodeGameStream(std::vector<uint8_t>& out, uint32_t tick, double time, const GameState& g);
void encodeAimStream(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<AimState>& a);

// ---------------------------------------------------------------- replies

void encodeOk(std::vector<uint8_t>& out, uint32_t requestId);
void encodeError(std::vector<uint8_t>& out, uint32_t requestId, uint16_t code, const std::string& message);
void encodeConsoleResult(std::vector<uint8_t>& out, uint32_t requestId, bool truncated,
                         const std::vector<std::string>& lines);
void encodeObjectList(std::vector<uint8_t>& out, uint32_t requestId, bool more, const std::vector<ObjectInfo>& objects);
void encodeObjectSpawned(std::vector<uint8_t>& out, uint32_t requestId, uint32_t objectId, uint32_t netId);
// ScriptList: the running scripts in start order, at most 255.
void encodeScriptList(std::vector<uint8_t>& out, uint32_t requestId,
                      const std::vector<std::pair<uint32_t, std::string> >& scripts);
void encodePing(std::vector<uint8_t>& out, uint16_t type, uint32_t requestId, uint64_t clock);

// ---------------------------------------------------------------- events

// Starts an event frame (type, sequence number, tick); the caller writes the fields and finish()es.
void beginEvent(proto::Writer& w, uint16_t type, uint32_t seq, uint32_t tick);

struct KillEvent
{
    uint8_t victim;
    int16_t attacker;
    uint8_t kind;
    std::string weapon;
    uint8_t victimTeam;
    uint32_t victimRootNetId;
    std::string victimRootTemplate;
    float victimPos[3];
    uint8_t attackerTeam;
    uint32_t attackerVehicleNetId;
    std::string attackerVehicleTemplate;
    float attackerPos[3];
    std::vector<std::pair<uint8_t, int32_t> > assists;
    KillEvent();
};

struct DamageEvent
{
    int16_t victimPlayer;
    uint32_t victimRootNetId, victimRootObjectId;
    std::string victimTemplate;
    int16_t attacker;
    std::string weapon, projectile;
    float amount, before, after, max;
    float pos[3];
    uint8_t flags;   // bit 0 destroyed, bit 1 killed a player
    DamageEvent();
};

struct VehicleSeatEvent   // EnterVehicle and ExitVehicle
{
    uint8_t player, seat, flags;
    uint32_t rootNetId, rootObjectId;
    std::string rootTemplate, seatTemplate;
    VehicleSeatEvent() : player(0), seat(kSeatNone), flags(0), rootNetId(0), rootObjectId(kNoObjectId) {}
};

struct ChatEvent
{
    int16_t sender;
    uint8_t team, channel;
    std::string channelName;
    uint32_t engineFlags;
    uint8_t textFlags;
    std::string text;
    ChatEvent() : sender(-1), team(0), channel(0), engineFlags(0), textFlags(0) {}
};

struct ProjectileHitEvent
{
    uint32_t id;
    std::string templateName;
    int16_t owner;
    uint8_t flags;   // bit 0 expired, bit 1 damaged something
    uint32_t firstVictimNetId;
    float pos[3];
    ProjectileHitEvent();
};

struct WeaponFireEvent
{
    int16_t shooter;
    uint32_t rootNetId;
    std::string weapon, projectile;
    uint8_t rounds;
    float origin[3], velocity[3];
    WeaponFireEvent();
};

void encodeGameStateEvent(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const GameState& g, uint8_t previous);
void encodeRoundStart(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const std::string& map,
                      const std::string& mode, const int32_t tickets[2]);
void encodeRoundEnd(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const int32_t tickets[2], uint8_t winner,
                    uint8_t victoryType);
void encodePlayerConnect(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const PlayerInfo& p);
void encodePlayerDisconnect(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t reason,
                            uint16_t engineReason);
void encodeSpawn(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t team, uint32_t rootNetId,
                 uint32_t rootObjectId, const std::string& rootTemplate, const float pos[3], const float rot[3]);
void encodeDeath(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t team, uint32_t netId,
                 const std::string& templateName, const float pos[3]);
void encodeRevive(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t revived, int16_t medic);
void encodeTeamChange(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t oldTeam,
                      uint8_t newTeam, uint8_t flags);
void encodeScoreChange(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const PlayerScore& s);
void encodeKill(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const KillEvent& k);
void encodeDamage(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const DamageEvent& d);
void encodeVehicleSeat(std::vector<uint8_t>& out, uint16_t type, uint32_t seq, uint32_t tick, const VehicleSeatEvent& e);
void encodeVehicleDestroyed(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t netId, uint32_t objectId,
                            const std::string& templateName, int16_t attacker, const float pos[3]);
void encodeControlPoint(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t kind, const ControlPointState& c);
void encodeChat(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ChatEvent& c);
void encodeProjectileFired(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileState& p);
void encodeProjectileHit(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileHitEvent& h);
void encodeKick(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const std::string& name);
void encodeBan(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, const std::string& name,
               uint8_t banType, uint32_t expiry);
void encodeBot(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player, uint8_t kind, uint8_t team,
               uint32_t connection);
void encodeWeaponFire(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const WeaponFireEvent& e);
void encodeWeapons(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint8_t player,
                   const std::vector<WeaponSlot>& slots);
// VoteResult. outcome: 1 passed, 2 failed, 3 cancelled by CancelVote, 4 cancelled by a level unload.
void encodeVoteResult(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t requestId,
                      const std::string& text, uint8_t outcome, uint16_t yes, uint16_t no, uint16_t eligible);

// ScriptFinished. reason: 1 finish, 2 StopScript, 3 error, 4 over its time budget, 5 level unload.
void encodeScriptFinished(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t instance,
                          const std::string& name, uint8_t reason, const std::string& result, uint32_t calls,
                          uint32_t totalUs, uint32_t maxUs);
// ScriptEvent. kind: 1 data (JSON text), 2 a log line. A payload beyond kMaxScriptPayload is cut.
void encodeScriptEvent(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, uint32_t instance, uint8_t kind,
                       const std::string& payload);

// ---------------------------------------------------------------- hit/miss replay (stream 20)

// ProjectileCollision kinds and flags.
const uint8_t kCollisionTerrain = 0, kCollisionObject = 1;
const uint8_t kCollisionPartIsRoot = 1, kCollisionPartIsSoldier = 2, kCollisionRewound = 4;
const size_t kMaxCollisionsPerTick = 64;

struct ProjectileCollisionEvent
{
    uint32_t id;                    // as ProjectileState's
    std::string templateName;
    int16_t shooter;
    uint8_t kind;                   // kCollisionTerrain or kCollisionObject
    uint8_t flags;                  // kCollision* bits
    uint32_t rootNetId;             // 0 none
    uint32_t rootObjectId;          // kNoObjectId none
    std::string rootTemplate;       // empty for terrain
    std::string partTemplate;       // the object touched; empty for terrain
    int16_t partPlayer;             // the soldier's player when kCollisionPartIsSoldier, else -1
    float pos[3], normal[3], speed[3];
    uint16_t material;
    float latency;                  // seconds
    float partPos[3], partRot[3];   // the part as tested
    float rootPos[3], rootRot[3];   // the root now
    ProjectileCollisionEvent();
};

// ProjectileTrace flags and limits.
const uint8_t kTraceCut = 1, kTraceCollided = 2;
const size_t kMaxTraceTargets = 8;
const size_t kMaxTraceSamples = 300;

struct TraceTarget
{
    uint32_t netId;
    std::string templateName;
};

struct TraceSample
{
    float pos[3], vel[3];   // the projectile
    float lag;              // the shooter's lag compensation, seconds
};

struct TracePose
{
    float pos[3], rot[3];               // the target now
    float testedPos[3], testedRot[3];   // the pose the projectile's sweep in this tick is tested against
};

struct ProjectileTraceEvent
{
    uint32_t id;
    std::string templateName;
    int16_t shooter;
    uint32_t firstTick;
    uint8_t flags;
    std::vector<TraceTarget> targets;
    std::vector<TraceSample> samples;
    std::vector<TracePose> poses;   // samples.size() x targets.size(), sample-major
    ProjectileTraceEvent();
};

void encodeProjectileCollision(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileCollisionEvent& e);
// Targets beyond kMaxTraceTargets and samples beyond kMaxTraceSamples are not written.
void encodeProjectileTrace(std::vector<uint8_t>& out, uint32_t seq, uint32_t tick, const ProjectileTraceEvent& e);

// ---------------------------------------------------------------- inbound (WebAdmin -> module)

// One parsed WebAdmin message. Fields not used by the type keep their defaults. The comments name
// the commands that use each field.
struct Command
{
    // Pseudo-types the I/O thread queues for link state changes; never on the wire.
    static const uint16_t LinkUp = 0x0001;
    static const uint16_t LinkDown = 0x0002;

    uint16_t type;
    uint32_t session;      // filled by the I/O thread
    uint32_t requestId;
    uint8_t player;        // the player id; Say: the team or player id
    uint8_t target;        // Say target, Map action, Ban "by", Subscribe/Unsubscribe stream, StartVote team
    uint8_t flags;         // Teleport, SetHealth, KillPlayer, SetTeam, Rank, SetCollision flags; SetScore mask; StartVote minimum ballots;
                           // StartScript flags
    uint8_t value8;        // SetTeam team, Rank rank, GameEvent/HudEvent event, Ban duration, Subscribe rate, SpawnObject team,
                           // StartVote percent, StartScript API version, SeatPreference seat
    int32_t ints[5];       // SetScore values; Medal medal and value; GameEvent/HudEvent data in [0]
    uint32_t seconds;      // Ban, StartVote
    uint16_t index;        // Map
    uint16_t size;         // Map
    uint16_t maxResults;   // ListObjects
    uint64_t clock;        // Ping, Pong
    ObjectRef ref;         // Teleport, SetHealth, SetObjectActive, DeleteObject
    float pos[3], rot[3];  // Teleport, SpawnObject
    float value;           // SetHealth
    std::string text;      // Console line, Say text, Kick/Ban reason, ListObjects/SpawnObject template, Map name,
                           // StartVote question, StartScript name
    std::string text2;     // Map mode; StartScript arguments and ScriptMessage message (JSON text, UTF-8)
    std::string source;    // StartScript source (UTF-8)
    uint32_t instance;     // StartScript, StopScript, ScriptMessage
    int16_t target16;      // HudEventRaw, RoundClock: the player, -1 every client
    std::vector<uint8_t> bytes;   // HudEventRaw payload
    float points[9];       // SquadOrder: three positions
    uint32_t netId;        // SquadOrder target networkable id, 0 none
    std::vector<std::string> filters;   // Subscribe
    std::string invalid;   // non-empty: a value check failed; the main thread answers Error 7 with it, in order
    Command();
};

// True for the types WebAdmin may send after the handshake.
bool isWebAdminMessage(uint16_t type);
// Parses a WebAdmin message after the handshake. Every payload must be exactly the message.
// Value checks that need no engine state (ranges, UTF-8, flag combinations) are done here too;
// such a failure returns false with *badValue set, which the caller reports as Error 7 for that
// request instead of a framing error.
bool parseCommand(uint16_t type, const uint8_t* p, size_t n, Command& out, std::string* badValue);

// ---------------------------------------------------------------- text

// Windows-1252 engine bytes to UTF-8. Control bytes become '?', except tab when keepTab is set.
std::string cp1252ToUtf8(const std::string& in, bool keepTab = false);
// UTF-8 to Windows-1252. @return False when in is not valid UTF-8 (overlong forms, surrogates and
// code points above U+10FFFF are invalid); a character without a 1252 byte becomes '?'.
bool utf8ToCp1252(const std::string& in, std::string& out);
// True when s contains a byte below 0x20 or 0x7F.
bool hasControlBytes(const std::string& s);

// Hex helpers for configuration and logs.
bool parseHex(const std::string& hex, std::vector<uint8_t>& out);
std::string toHex(const uint8_t* data, size_t n);

} // namespace admin
} // namespace serverext

#endif // SERVEREXT_ADMINPROTOCOL_H
