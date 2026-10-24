// Wire format of the bot channel, docs/protocol.md v3 (layouts in docs/findings/module-v2.md and module-v3.md). Engine-free: the I/O thread, the main thread
// and the unit tests all use it.

#ifndef SERVEREXT_PROTOCOL_H
#define SERVEREXT_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace serverext {
namespace proto {

const uint16_t kVersion = 3;
const size_t kHeaderSize = 8;
const uint32_t kMaxPayload = 64 * 1024;

enum MsgType : uint16_t
{
    MsgHello        = 0x0001,
    MsgWelcome      = 0x0002,
    MsgCreateBot    = 0x0101,
    MsgBotCreated   = 0x0102,
    MsgEnterVehicle = 0x0103,
    MsgRemoveBot    = 0x0104,
    MsgAck          = 0x0105,
    MsgBotRemoved   = 0x0106,
    MsgSpawnBlocked = 0x0107,
    MsgRestartRound = 0x0108,
    MsgTeleport     = 0x0109,
    MsgSpot         = 0x010A,
    MsgSay          = 0x010B,   // bot chat (no version change, approved 2026-10-24)
    MsgSetInput     = 0x0201,
    MsgSetTargetDirection = 0x0202,   // v3 item 3
    MsgTickState    = 0x0301,
    MsgVehicleList  = 0x0302,
    MsgProjectileList = 0x0303,
    MsgDamage       = 0x0304,
    MsgKill         = 0x0305,
    MsgChat         = 0x0306,
    MsgRoundState   = 0x0307,
    MsgError        = 0xFFFF
};

enum ErrorCode : uint16_t
{
    ErrVersion       = 1,
    ErrMalformed     = 2,
    ErrUnknownType   = 3,
    ErrUnknownPlayer = 4,
    ErrUnavailable   = 5,
    ErrRefused       = 6,
    ErrInvalidValue  = 7
};

enum RemoveReason : uint8_t
{
    ReasonRemoveBot = 1,
    ReasonMapChange = 2,
    ReasonEngine    = 3
};

// TickState entry flags.
const uint8_t kFlagAlive     = 1;
const uint8_t kFlagOwnBot    = 2;
const uint8_t kFlagInVehicle = 4;
const uint8_t kFlagManDown   = 8;    // v2 item 10
const uint8_t kFlagSpotted   = 16;   // v2 item 6: spotted by the entry's enemies within the last 12 s
const uint8_t kViewNone      = 255;
const uint8_t kSeatOnFoot    = 255;

// Appends little-endian values to a byte buffer. begin() writes a frame header whose length
// finish() patches, so several frames can be appended to one buffer.
class Writer
{
public:
    explicit Writer(std::vector<uint8_t>& out) : m_out(out), m_start(0) {}

    void begin(uint16_t type);
    void finish();

    void u8(uint8_t v) { m_out.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void f32(float v);
    void f64(double v);
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v);
    void bytes(const uint8_t* data, size_t n) { m_out.insert(m_out.end(), data, data + n); }
    // u16 byte length then the bytes; longer strings are truncated to 65535 bytes.
    void str(const std::string& s);

private:
    std::vector<uint8_t>& m_out;
    size_t m_start;
};

// Reads little-endian values from a payload. Any read past the end sets a sticky failure and
// returns zeros, so a parser can read every field and check ok() once.
class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0), m_ok(true) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    float f32();
    int16_t i16() { return static_cast<int16_t>(u16()); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64();
    double f64();
    // Copies n raw bytes into out (zeros on failure).
    void bytes(uint8_t* out, size_t n);
    void str(std::string& out);

    bool ok() const { return m_ok; }
    // True when every byte was consumed and no read failed.
    bool done() const { return m_ok && m_pos == m_size; }

private:
    bool need(size_t n);

    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
    bool m_ok;
};

struct Frame
{
    uint16_t type;
    std::vector<uint8_t> payload;
};

// Splits a byte stream into frames. The buffer never holds more than one maximum frame plus one
// read chunk, because next() is called after every feed().
class FrameDecoder
{
public:
    enum Result { NeedMore, GotFrame, Oversize, BadReserved };

    // maxPayload: the largest accepted payload (the bot channel's 64 KiB by default).
    explicit FrameDecoder(uint32_t maxPayload = kMaxPayload) : m_pos(0), m_max(maxPayload) {}

    void feed(const uint8_t* data, size_t size);
    // Extracts the next complete frame. Oversize and BadReserved are terminal for the stream.
    Result next(Frame& out);
    void reset() { m_buf.clear(); m_pos = 0; }
    size_t buffered() const { return m_buf.size() - m_pos; }

private:
    std::vector<uint8_t> m_buf;
    size_t m_pos;
    uint32_t m_max;
};

struct HelloMsg        { uint16_t version; std::string name; };
struct CreateBotMsg    { uint32_t requestId; std::string name; uint8_t team; };
struct EnterVehicleMsg { uint32_t requestId; uint8_t playerId; uint32_t vehicleId; uint8_t seat; };
struct RemoveBotMsg    { uint32_t requestId; uint8_t playerId; };
struct RestartRoundMsg { uint32_t requestId; };
// v2 item 9: moves a bot's root object (its vehicle when seated) to pos with the given yaw, level.
struct TeleportMsg     { uint32_t requestId; uint8_t playerId; float pos[3]; float yaw; };
// v2 item 6: the bot spots the root object with networkable id targetId.
struct SpotMsg         { uint32_t requestId; uint8_t playerId; uint32_t targetId; };
// The bot sends a chat line as itself on channel 0 (Global) or 1 (Team).
struct SayMsg          { uint32_t requestId; uint8_t playerId; uint8_t channel; std::string text; };

// Axis order on the wire: cyclic pitch, cyclic roll, collective, pedals, look yaw, look pitch.
enum Axis
{
    AxisPitch = 0, AxisRoll = 1, AxisCollective = 2, AxisPedals = 3, AxisLookYaw = 4, AxisLookPitch = 5,
    AxisCount = 6
};

struct SetInputMsg
{
    uint8_t playerId;
    uint32_t tick;
    float axes[AxisCount];
    uint32_t buttons;
};

// v3 item 3: steer the bot's TV-guided weapon along the ray from start through end (world metres).
struct SetTargetDirectionMsg
{
    uint8_t playerId;
    uint32_t tick;
    float start[3];
    float end[3];
};

// Each parser requires the payload to be exactly the message, with no trailing bytes.
bool parseHello(const std::vector<uint8_t>& p, HelloMsg& out);
bool parseCreateBot(const std::vector<uint8_t>& p, CreateBotMsg& out);
bool parseEnterVehicle(const std::vector<uint8_t>& p, EnterVehicleMsg& out);
bool parseRemoveBot(const std::vector<uint8_t>& p, RemoveBotMsg& out);
bool parseSetInput(const std::vector<uint8_t>& p, SetInputMsg& out);
bool parseRestartRound(const std::vector<uint8_t>& p, RestartRoundMsg& out);
bool parseTeleport(const std::vector<uint8_t>& p, TeleportMsg& out);
bool parseSpot(const std::vector<uint8_t>& p, SpotMsg& out);
bool parseSay(const std::vector<uint8_t>& p, SayMsg& out);
bool parseSetTargetDirection(const std::vector<uint8_t>& p, SetTargetDirectionMsg& out);

// Checks a SetTargetDirection's values (v3 item 3): finite, both points inside the world (|x| and
// |z| at most 8192, y in -1000..10000) and at least 0.1 m apart. Returns null when valid, else the
// reason for the Error 7 message.
const char* checkTargetDirection(const SetTargetDirectionMsg& m);

// Checks a Say's values: channel 0 or 1, text 1..kMaxSayText bytes of printable ASCII (0x20-0x7E).
// Returns null when valid, else the reason for the Error 7 message.
const size_t kMaxSayText = 100;
const char* checkSay(const SayMsg& m);

// True for the message types a sidecar may send.
bool isClientMessage(uint16_t type);

// Welcome feature bits (v2).
const uint8_t kFeatureTeleport = 1;
const uint8_t kFeatureRoundRestart = 2;

void encodeWelcome(std::vector<uint8_t>& out, float tickRate, uint32_t tick, const std::string& map,
                   const std::string& mode, float enterRange, float projectileRange, uint8_t features);
void encodeBotCreated(std::vector<uint8_t>& out, uint32_t requestId, uint8_t playerId);
void encodeAck(std::vector<uint8_t>& out, uint32_t requestId);
void encodeBotRemoved(std::vector<uint8_t>& out, uint8_t playerId, uint8_t reason);
void encodeSpawnBlocked(std::vector<uint8_t>& out, uint8_t playerId, uint16_t secondsWaiting);
// Messages longer than 1024 bytes are truncated.
void encodeError(std::vector<uint8_t>& out, uint32_t requestId, uint16_t code, const std::string& message);

// TickState weapon flags (v2 item 4).
const uint8_t kWeaponReloading = 1;
const uint8_t kWeaponSelected = 2;

struct WeaponState
{
    uint8_t slot;            // 0 fire, 1 alt fire, 2 flares
    std::string templateName;
    int16_t ammoInMag;       // -1 unknown or infinite
    int16_t mags;            // including the loaded one; -1 unknown
    uint8_t flags;
    float reloadLeft;        // 0..1 of the reload still to go
    float heat;              // 0..1, -1 when the weapon has no heat
};

const int kMaxWeapons = 3;

struct StateEntry
{
    uint8_t playerId;
    uint8_t flags;
    uint8_t team;
    uint8_t seat;
    uint32_t rootId;
    float pos[3];
    float ypr[3];
    float vel[3];
    float angVel[3];
    float health;
    uint8_t viewMode;        // v2 item 1; kViewNone for other players
    uint8_t weaponCount;     // v2 item 4; 0..kMaxWeapons, 0 for other players
    WeaponState weapons[kMaxWeapons];
};

// At most 255 entries are written; the rest are dropped. Template names are cut to 255 bytes.
void encodeTickState(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<StateEntry>& entries);

// VehicleList entry flags.
const uint8_t kVehicleDestroyed = 1;

struct VehicleEntry
{
    uint32_t netId;
    std::string templateName;
    uint8_t team;
    uint8_t flags;
    float pos[3];
    float yaw;
    float health;
    uint8_t seats;
    uint32_t occupied;   // bit n = seat n
};

// Template names longer than 255 bytes are truncated. Entries that would take the payload past
// kMaxPayload (or past 65535 entries) are dropped from the end.
void encodeVehicleList(std::vector<uint8_t>& out, uint32_t tick, const std::vector<VehicleEntry>& entries);

// ProjectileList (v2 item 5).
const uint8_t kProjectileGuided = 1;
const uint8_t kProjectileOwn = 2;
const uint8_t kProjectileHasOwner = 4;   // player ids count down from 255, so no id value means none
const size_t kMaxProjectiles = 64;

struct ProjectileEntry
{
    uint32_t id;             // networkable id, or engine object id + 0x10000 without one
    std::string templateName;
    uint8_t owner;           // valid with kProjectileHasOwner
    uint8_t team;
    uint8_t flags;
    uint32_t targetId;       // networkable id of the target's root, 0 none
    float pos[3];
    float vel[3];
    float timeToLive;
    uint32_t launch;         // launch number, from LaunchCounter
};

// ProjectileList launch numbers: 1, 2, 3, ... for every launch the module tracks since it loaded.
// The projectile id is reused across flights (the engine pools projectile objects); this is not.
class LaunchCounter
{
public:
    explicit LaunchCounter(uint32_t last = 0) : m_last(last) {}
    uint32_t next() { return ++m_last ? m_last : ++m_last; }   // 0 is never handed out
private:
    uint32_t m_last;
};

// At most kMaxProjectiles entries are written.
void encodeProjectileList(std::vector<uint8_t>& out, uint32_t tick, const std::vector<ProjectileEntry>& entries);

// Damage flags (v2 item 8).
const uint8_t kDamageVictimOwn = 1;
const uint8_t kDamageAttackerOwn = 2;
const uint8_t kDamageVictimDestroyed = 4;
const uint8_t kDamageKilled = 8;
const uint8_t kDamageHasVictimPlayer = 16;
const uint8_t kDamageHasAttacker = 32;

struct DamageEvent
{
    uint32_t tick;
    uint8_t flags;
    uint8_t victim;          // player id, valid with kDamageHasVictimPlayer
    uint32_t victimRootId;
    std::string victimTemplate;
    uint8_t attacker;        // player id, valid with kDamageHasAttacker
    std::string weaponTemplate;
    std::string sourceTemplate;
    float amount;
    float before;
    float after;
    float max;
    float pos[3];
};

void encodeDamage(std::vector<uint8_t>& out, const DamageEvent& e);

// Kill flags (v2 item 8).
const uint8_t kKillVictimOwn = 1;
const uint8_t kKillAttackerOwn = 2;
const uint8_t kKillTeamKill = 4;
const uint8_t kKillHasAttacker = 8;

struct KillEvent
{
    uint32_t tick;
    uint8_t victim;
    uint8_t victimTeam;
    uint8_t attacker;        // valid with kKillHasAttacker
    uint8_t attackerTeam;
    std::string weaponTemplate;
    uint8_t flags;
};

void encodeKill(std::vector<uint8_t>& out, const KillEvent& e);

// Chat text is the engine's Windows-1252, sent as UTF-8 like the admin channel's (control bytes become
// '?'), cut to at most 255 bytes on a character boundary (v2 item 7). sender is valid with kChatFromPlayer.
const uint8_t kChatFromPlayer = 1;
void encodeChat(std::vector<uint8_t>& out, uint32_t tick, uint8_t flags, uint8_t sender, uint8_t team, uint8_t channel,
                const std::string& text);

// RoundState (v2 item 7). 255 = unknown for the two states.
void encodeRoundState(std::vector<uint8_t>& out, uint32_t tick, uint8_t gameStatus, uint8_t beginRoundState,
                      float timeToNextChange);

// The engine's convertAxisToInternalResolution (Game/Common/PlayerAction.cpp).
float convertAxisToInternalResolution(float value);

// What a human's axis value becomes on the server: PlayerAction::setAxis stores
// short(int(convertAxisToInternalResolution(v) * 100)) and PlayerAction::getAxis yields that / 100.
float quantizeLikeHuman(float value);

// Validates and conditions one SetInput axis: rejects NaN and infinity, clamps to -1..1, then
// quantizes like a human's input. Returns false for a rejected value and leaves out unchanged.
bool conditionAxis(float value, float& out);

} // namespace proto
} // namespace serverext

#endif // SERVEREXT_PROTOCOL_H
