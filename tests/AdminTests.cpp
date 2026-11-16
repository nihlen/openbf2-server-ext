// Unit tests for the admin channel: the codec, text conversion, rotations, configuration, the
// main/I-O queue, the handshake and command parsing (including hostile input), and the I/O thread
// against a local TLS 1.3 server (right pin, wrong pin, plain loopback, reconnect). No engine.

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "AdminCrypto.h"
#include "AdminIo.h"
#include "AdminLink.h"
#include "AdminProtocol.h"
#include "AdminSession.h"
#include "Config.h"
#include "Health.h"
#include "Rotation.h"

using namespace serverext;
using namespace serverext::admin;
using serverext::proto::Reader;
using serverext::proto::Writer;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

std::vector<uint8_t> frame(uint16_t type, const std::vector<uint8_t>& payload, uint16_t reserved = 0)
{
    std::vector<uint8_t> out;
    Writer w(out);
    w.begin(type);
    out.insert(out.end(), payload.begin(), payload.end());
    w.finish();
    out[6] = static_cast<uint8_t>(reserved);
    out[7] = static_cast<uint8_t>(reserved >> 8);
    return out;
}

// Splits a buffer of frames.
std::vector<proto::Frame> frames(const std::vector<uint8_t>& bytes)
{
    proto::FrameDecoder d(kMaxOutboundPayload);
    d.feed(bytes.empty() ? 0 : &bytes[0], bytes.size());
    std::vector<proto::Frame> out;
    proto::Frame f;
    while (d.next(f) == proto::FrameDecoder::GotFrame)
        out.push_back(f);
    return out;
}

std::vector<uint8_t> key16()
{
    std::vector<uint8_t> k;
    for (int i = 0; i < 32; ++i)
        k.push_back(static_cast<uint8_t>(0xA0 + i));
    return k;
}

const char* const kServerId = "127.0.0.1:16573";

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------- text and helpers

void testText()
{
    // Windows-1252 to UTF-8: the colour prefix, the euro sign, an undefined byte and a control byte.
    CHECK(cp1252ToUtf8("\xA7" "C1001x") == "\xC2\xA7" "C1001x");
    CHECK(cp1252ToUtf8("\x80") == "\xE2\x82\xAC");
    CHECK(cp1252ToUtf8("\x81") == "\xC2\x81");
    CHECK(cp1252ToUtf8("a\x01" "b\tc") == "a?b?c");
    CHECK(cp1252ToUtf8("a\tb", true) == "a\tb");
    CHECK(cp1252ToUtf8("\xE9t\xE9") == "\xC3\xA9t\xC3\xA9");

    std::string out;
    CHECK(utf8ToCp1252("\xC2\xA7" "C1001", out) && out == "\xA7" "C1001");
    CHECK(utf8ToCp1252("\xE2\x82\xAC", out) && out == "\x80");
    CHECK(utf8ToCp1252("\xC2\x81", out) && out == "\x81");
    CHECK(utf8ToCp1252("\xC2\x80", out) && out == "?");             // U+0080 has no 1252 byte
    CHECK(utf8ToCp1252("\xE4\xB8\xAD", out) && out == "?");         // CJK
    CHECK(utf8ToCp1252("\xF0\x9F\x98\x80", out) && out == "?");     // emoji
    CHECK(!utf8ToCp1252("\xC0\xAF", out));                          // overlong
    CHECK(!utf8ToCp1252("\xED\xA0\x80", out));                      // surrogate
    CHECK(!utf8ToCp1252("\xF4\x90\x80\x80", out));                  // above U+10FFFF
    CHECK(!utf8ToCp1252("\xC3", out));                              // truncated
    CHECK(!utf8ToCp1252("\x80", out));                              // lone continuation
    CHECK(!utf8ToCp1252("\xFF", out));
    // Every 1252 byte from 0x20 survives the round trip.
    for (int b = 0x20; b < 0x100; ++b)
    {
        if (b == 0x7F)
            continue;
        const std::string one(1, static_cast<char>(b));
        CHECK(utf8ToCp1252(cp1252ToUtf8(one), out) && out == one);
    }
    CHECK(hasControlBytes("a\nb") && hasControlBytes("\x7F") && !hasControlBytes("\xA7" "abc"));

    std::vector<uint8_t> raw;
    CHECK(parseHex("00ff10Ab", raw) && raw.size() == 4 && raw[1] == 0xFF && raw[3] == 0xAB);
    CHECK(!parseHex("0", raw) && !parseHex("zz", raw));
    const uint8_t bytes[3] = { 0x00, 0x7F, 0xFE };
    CHECK(toHex(bytes, 3) == "007ffe");
}

void testCrypto()
{
    // RFC 4231 test case 2.
    const std::string k = "Jefe", d = "what do ya want for nothing?";
    uint8_t mac[32];
    CHECK(hmacSha256(std::vector<uint8_t>(k.begin(), k.end()), std::vector<uint8_t>(d.begin(), d.end()), mac));
    CHECK(toHex(mac, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t a[32], b[32];
    CHECK(randomBytes(a, 32) && randomBytes(b, 32) && !equalConstantTime(a, b, 32));
    CHECK(equalConstantTime(a, a, 32));
    uint8_t h[32];
    sha256(reinterpret_cast<const uint8_t*>("abc"), 3, h);
    CHECK(toHex(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // The transcript layout.
    uint8_t nm[32], nw[32];
    for (int i = 0; i < 32; ++i) { nm[i] = static_cast<uint8_t>(i); nw[i] = static_cast<uint8_t>(0xFF - i); }
    const std::vector<uint8_t> t = proofTranscript(kServerProofLabel, "ab", nm, nw);
    const std::string label = kServerProofLabel;
    CHECK(t.size() == label.size() + 1 + 2 + 2 + 64);
    CHECK(std::string(t.begin(), t.begin() + label.size()) == label && t[label.size()] == 0);
    CHECK(t[label.size() + 1] == 2 && t[label.size() + 2] == 0 && t[label.size() + 3] == 'a');
    CHECK(t[label.size() + 5] == 0 && t.back() == 0xE0);
}

void testRotation()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> yaw(-179.0f, 179.0f), pitch(-85.0f, 85.0f), roll(-179.0f, 179.0f);
    for (int i = 0; i < 2000; ++i)
    {
        float in[3] = { yaw(rng), pitch(rng), roll(rng) };
        float m[16];
        for (int k = 0; k < 16; ++k)
            m[k] = 0.0f;
        matrixFromAttitude(in, m);
        float out[3];
        attitudeFromMatrix(m, out);
        CHECK(near(in[0], out[0], 0.02f) && near(in[1], out[1], 0.02f) && near(in[2], out[2], 0.02f));
        // Orthonormal rows.
        const float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
        const float n2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
        CHECK(std::fabs(d01) < 1e-5f && std::fabs(n2 - 1.0f) < 1e-5f);
    }
    // Conventions: yaw 90 points the nose east (+x); pitch +10 is nose down; roll +20 raises the right.
    float m[16] = { 0 };
    float ypr[3] = { 90.0f, 0.0f, 0.0f };
    matrixFromAttitude(ypr, m);
    CHECK(near(m[8], 1.0f) && near(m[10], 0.0f));
    float p[3] = { 0.0f, 10.0f, 0.0f };
    matrixFromAttitude(p, m);
    CHECK(m[9] < 0.0f);
    float r[3] = { 0.0f, 0.0f, 20.0f };
    matrixFromAttitude(r, m);
    CHECK(m[1] > 0.0f);
}

// ---------------------------------------------------------------- encoders

PlayerState samplePlayerState()
{
    PlayerState s;
    s.team = 2; s.squad = 3; s.life = kLifeManDown; s.flags = kPlayerInVehicle | kPlayerSquadLeader; s.ping = 48;
    s.rootNetId = 1819; s.rootObjectId = 29328; s.seat = 1;
    s.pos[0] = 1.5f; s.pos[1] = 100.25f; s.pos[2] = -3.0f;
    s.rot[0] = 90.0f; s.rot[1] = -5.0f; s.rot[2] = 2.5f;
    s.vel[0] = 10.0f; s.vel[1] = 0.5f; s.vel[2] = -1.0f;
    s.health = 0.75f;
    s.viewMode = 3;
    s.lagMs = 245;
    return s;
}

bool samePlayerState(const PlayerState& a, const PlayerState& b)
{
    for (int k = 0; k < 3; ++k)
        if (a.pos[k] != b.pos[k] || a.rot[k] != b.rot[k] || a.vel[k] != b.vel[k])
            return false;
    return a.team == b.team && a.squad == b.squad && a.life == b.life && a.flags == b.flags && a.ping == b.ping &&
           a.rootNetId == b.rootNetId && a.rootObjectId == b.rootObjectId && a.seat == b.seat && a.health == b.health &&
           a.viewMode == b.viewMode && a.lagMs == b.lagMs;
}

PlayerInfo samplePlayerInfo()
{
    PlayerInfo p;
    p.id = 3;
    p.name = "\xC2\xA7" "netsky";
    p.flags = kInfoConnected | kInfoIdentity;
    p.profileId = 43001234;
    p.ip = "10.0.0.7";
    p.hash = "0123456789abcdef0123456789abcdef";
    p.score.score = 12; p.score.teamScore = 4; p.score.kills = 5; p.score.deaths = 2; p.score.teamKills = 1;
    p.score.rank = 7;
    p.state = samplePlayerState();
    return p;
}

VehicleState sampleVehicle()
{
    VehicleState v;
    v.netId = 1820; v.objectId = 31000; v.templateName = "ahe_ah1z"; v.team = 1; v.flags = 0;
    v.pos[0] = 4.0f; v.pos[1] = 50.0f; v.pos[2] = 6.0f;
    v.rot[0] = 45.0f; v.rot[1] = 1.0f; v.rot[2] = -1.0f;
    v.vel[0] = 0.0f; v.vel[1] = -0.5f; v.vel[2] = 3.0f;
    v.health = 0.9f; v.seats = 2; v.occupied = 1;
    return v;
}

void testStructures()
{
    std::vector<uint8_t> buf;
    Writer w(buf);
    const PlayerState s = samplePlayerState();
    writePlayerState(w, s);
    CHECK(buf.size() == 1 + 1 + 1 + 1 + 2 + 4 + 4 + 1 + 36 + 4 + 1 + 2);
    {
        Reader r(&buf[0], buf.size());
        PlayerState back;
        readPlayerState(r, back);
        CHECK(r.done() && samePlayerState(s, back));
    }

    buf.clear();
    GameState g;
    g.status = 1; g.beginRound = 3; g.timeToNext = 12.5f; g.tickets[0] = 150; g.tickets[1] = 0; g.roundTime = 61.0f;
    g.timeLimit = 1800.0f; g.winner = 2; g.victoryType = 3;
    writeGameState(w, g);
    CHECK(buf.size() == 1 + 1 + 4 + 4 + 4 + 4 + 4 + 1 + 1);
    {
        Reader r(&buf[0], buf.size());
        GameState back;
        readGameState(r, back);
        CHECK(r.done() && back.status == 1 && back.beginRound == 3 && back.timeToNext == 12.5f && back.tickets[0] == 150 &&
              back.tickets[1] == 0 && back.roundTime == 61.0f && back.timeLimit == 1800.0f && back.winner == 2 &&
              back.victoryType == 3);
    }

    buf.clear();
    const PlayerInfo p = samplePlayerInfo();
    writePlayerInfo(w, p);
    {
        Reader r(&buf[0], buf.size());
        PlayerInfo back;
        readPlayerInfo(r, back);
        CHECK(r.done() && back.id == 3 && back.name == p.name && back.flags == p.flags && back.profileId == p.profileId &&
              back.ip == p.ip && back.hash == p.hash && back.score == p.score && samePlayerState(back.state, p.state));
    }

    buf.clear();
    const VehicleState v = sampleVehicle();
    writeVehicleState(w, v);
    CHECK(buf.size() == 4 + 4 + 2 + v.templateName.size() + 1 + 1 + 36 + 4 + 1 + 4);
    {
        Reader r(&buf[0], buf.size());
        VehicleState back;
        readVehicleState(r, back);
        CHECK(r.done() && back.netId == 1820 && back.objectId == 31000 && back.templateName == "ahe_ah1z" &&
              back.team == 1 && back.pos[1] == 50.0f && back.rot[0] == 45.0f && back.vel[2] == 3.0f &&
              back.health == 0.9f && back.seats == 2 && back.occupied == 1);
    }

    buf.clear();
    ControlPointState c;
    c.objectId = 400; c.netId = 0; c.templateName = "cpname_dalian_harbour"; c.team = 2; c.flagTeam = 2; c.takeOver = 0.5f;
    c.capturing = 1; c.pos[0] = 1; c.pos[1] = 2; c.pos[2] = 3;
    writeControlPointState(w, c);
    {
        Reader r(&buf[0], buf.size());
        ControlPointState back;
        readControlPointState(r, back);
        CHECK(r.done() && back.objectId == 400 && back.templateName == c.templateName && back.team == 2 &&
              back.takeOver == 0.5f && back.capturing == 1 && back.pos[2] == 3.0f);
    }

    buf.clear();
    ProjectileState pr;
    pr.id = 0x10000 + 77; pr.templateName = "agm114_hellfire_tv"; pr.owner = 254; pr.team = 1; pr.flags = 1;
    pr.targetNetId = 1820; pr.timeToLive = 9.5f;
    writeProjectileState(w, pr);
    {
        Reader r(&buf[0], buf.size());
        ProjectileState back;
        readProjectileState(r, back);
        CHECK(r.done() && back.id == pr.id && back.templateName == pr.templateName && back.owner == 254 &&
              back.flags == 1 && back.targetNetId == 1820 && back.timeToLive == 9.5f);
    }
    // A missing owner is -1, not 255: bots count down from 255.
    pr.owner = -1;
    buf.clear();
    writeProjectileState(w, pr);
    {
        Reader r(&buf[0], buf.size());
        ProjectileState back;
        readProjectileState(r, back);
        CHECK(back.owner == -1);
    }
}

void testSessionMessages()
{
    uint8_t nonce[32];
    for (int i = 0; i < 32; ++i)
        nonce[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> out;
    encodeHello(out, kServerId, nonce, "build");
    std::vector<proto::Frame> f = frames(out);
    CHECK(f.size() == 1 && f[0].type == MsgHello);
    {
        Reader r(&f[0].payload[0], f[0].payload.size());
        std::string id, build;
        CHECK(r.u16() == 1);
        r.str(id);
        uint8_t back[32];
        r.bytes(back, 32);
        r.str(build);
        CHECK(r.done() && id == kServerId && back[31] == 31 && build == "build");
    }

    WelcomeInfo w;
    w.session = 3; w.tick = 900; w.serverName = "serverext admin"; w.gamePort = 16573; w.gameSpyPort = 29905;
    w.maxPlayers = 16; w.capabilities = kCapConsole | kCapObjects; w.map = "dalian_2_v_2"; w.mode = "gpm_cq";
    w.mapSize = 16; w.mapIndex = 0;
    MapEntry e;
    e.name = "dalian_2_v_2"; e.mode = "gpm_cq"; e.size = 16;
    w.maps.push_back(e);
    e.name = "daqing_2_v_2";
    w.maps.push_back(e);
    w.team1 = "CH"; w.team2 = "US";
    w.game.status = 1;
    out.clear();
    encodeWelcome(out, w);
    f = frames(out);
    CHECK(f.size() == 1 && f[0].type == MsgWelcome);
    {
        Reader r(&f[0].payload[0], f[0].payload.size());
        std::string s;
        CHECK(r.u16() == 1 && r.u32() == 3 && r.u32() == 900 && r.f32() == 30.0f);
        r.str(s);
        CHECK(s == "serverext admin" && r.u16() == 16573 && r.u16() == 29905 && r.u8() == 16 && r.u8() == 3);
        r.str(s);
        CHECK(s == "dalian_2_v_2");
        r.str(s);
        CHECK(s == "gpm_cq" && r.u16() == 16 && r.u16() == 0 && r.u16() == 2);
        for (int i = 0; i < 2; ++i)
        {
            r.str(s);
            r.str(s);
            CHECK(r.u16() == 16);
        }
        r.str(s);
        CHECK(s == "CH");
        r.str(s);
        CHECK(s == "US");
        GameState g;
        readGameState(r, g);
        CHECK(r.done() && g.status == 1);
    }

    ChallengeMsg c;
    std::vector<uint8_t> payload(64, 0x5A);
    CHECK(parseChallenge(&payload[0], 64, c) && c.proof[31] == 0x5A);
    CHECK(!parseChallenge(&payload[0], 63, c));
    payload.push_back(0);
    CHECK(!parseChallenge(&payload[0], 65, c));
}

void testSnapshotAndStreams()
{
    GameState g;
    g.status = 1;
    std::vector<PlayerInfo> players(2, samplePlayerInfo());
    players[1].id = 255;
    players[1].flags = kInfoAi | kInfoBotChannel;
    players[1].ip.clear();
    players[1].hash.clear();
    std::vector<VehicleState> vehicles(3, sampleVehicle());
    std::vector<ControlPointState> cps(1);
    std::vector<ProjectileState> projectiles;
    std::vector<uint8_t> out;
    encodeSnapshot(out, 17, 42, 1000, 33.5, g, players, vehicles, cps, projectiles);
    std::vector<proto::Frame> f = frames(out);
    CHECK(f.size() == 1 && f[0].type == MsgSnapshot);
    {
        Reader r(&f[0].payload[0], f[0].payload.size());
        CHECK(r.u32() == 17 && r.u32() == 42 && r.u32() == 1000 && r.f64() == 33.5);
        GameState gb;
        readGameState(r, gb);
        CHECK(r.u8() == 2);
        PlayerInfo p;
        readPlayerInfo(r, p);
        readPlayerInfo(r, p);
        CHECK(p.id == 255 && p.flags == (kInfoAi | kInfoBotChannel) && p.ip.empty());
        CHECK(r.u16() == 3);
        VehicleState v;
        for (int i = 0; i < 3; ++i)
            readVehicleState(r, v);
        CHECK(r.u16() == 1);
        ControlPointState c;
        readControlPointState(r, c);
        CHECK(r.u16() == 0 && r.done());
    }

    std::vector<uint8_t> ids(1, 7);
    std::vector<PlayerState> states(1, samplePlayerState());
    out.clear();
    encodePlayerStream(out, 5, 1.0, ids, states);
    f = frames(out);
    CHECK(f.size() == 1 && f[0].type == MsgPlayerStream && f[0].payload.size() == 4 + 8 + 1 + 1 + 58);
    out.clear();
    encodeVehicleStream(out, 5, 1.0, vehicles);
    encodeProjectileStream(out, 5, 1.0, projectiles);
    encodeGameStream(out, 5, 1.0, g);
    f = frames(out);
    CHECK(f.size() == 3 && f[0].type == MsgVehicleStream && f[1].type == MsgProjectileStream && f[2].type == MsgGameStream);
    CHECK(f[1].payload.size() == 4 + 8 + 2 && f[2].payload.size() == 4 + 8 + 24);

    // A 64-player stream frame stays well under the 1 MiB limit.
    std::vector<uint8_t> manyIds(64);
    std::vector<PlayerState> many(64, samplePlayerState());
    out.clear();
    encodePlayerStream(out, 5, 1.0, manyIds, many);
    CHECK(out.size() == 8 + 13 + 64 * 59);

    // Aim: 50 bytes per living player.
    std::vector<AimState> aims(2);
    aims[0].player = 7; aims[0].flags = kAimCamera | kAimWeapon;
    aims[0].cameraPos[1] = 100.0f; aims[0].weaponRot[0] = 90.0f;
    aims[1].player = 255;
    out.clear();
    encodeAimStream(out, 5, 1.0, aims);
    f = frames(out);
    CHECK(f.size() == 1 && f[0].type == MsgAimStream && f[0].payload.size() == 4 + 8 + 1 + 2 * 50);
    {
        Reader r(&f[0].payload[0], f[0].payload.size());
        CHECK(r.u32() == 5 && r.f64() == 1.0 && r.u8() == 2 && r.u8() == 7 && r.u8() == 3);
        float v[3];
        readVec3(r, v);
        CHECK(v[1] == 100.0f);
        readVec3(r, v);
        readVec3(r, v);
        readVec3(r, v);
        CHECK(v[0] == 90.0f && r.u8() == 255 && r.u8() == 0);
    }
}

void testEvents()
{
    std::vector<uint8_t> out;
    GameState g;
    encodeGameStateEvent(out, 1, 10, g, 3);
    const int32_t tickets[2] = { 100, 90 };
    encodeRoundStart(out, 2, 10, "dalian_2_v_2", "gpm_cq", tickets);
    encodeRoundEnd(out, 3, 10, tickets, 1, 2);
    encodePlayerConnect(out, 4, 10, samplePlayerInfo());
    encodePlayerDisconnect(out, 5, 10, 3, DiscKicked, 21);
    const float pos[3] = { 1, 2, 3 }, rot[3] = { 4, 5, 6 };
    encodeSpawn(out, 6, 10, 3, 2, 1800, 5000, "us_heavy_soldier", pos, rot);
    encodeDeath(out, 7, 10, 3, 2, 1800, "us_heavy_soldier", pos);
    encodeRevive(out, 8, 10, 3, -1);
    encodeTeamChange(out, 9, 10, 255, 2, 1, 1);
    encodeScoreChange(out, 10, 10, 3, samplePlayerInfo().score);
    KillEvent k;
    k.victim = 3; k.attacker = 255; k.kind = KillNormal; k.weapon = "ahe_z10_S8Launcher"; k.victimTeam = 2;
    k.victimRootNetId = 1800; k.victimRootTemplate = "us_heavy_soldier"; k.attackerTeam = 1; k.attackerVehicleNetId = 1819;
    k.attackerVehicleTemplate = "ahe_z10";
    k.assists.push_back(std::make_pair(static_cast<uint8_t>(254), 2));
    encodeKill(out, 11, 10, k);
    DamageEvent d;
    d.victimPlayer = 3; d.attacker = 255; d.weapon = "w"; d.projectile = "p"; d.amount = 10; d.before = 100; d.after = 90;
    d.max = 100;
    encodeDamage(out, 12, 10, d);
    VehicleSeatEvent s;
    s.player = 255; s.seat = 0; s.rootNetId = 1819; s.rootObjectId = 29328; s.rootTemplate = "ahe_z10"; s.seatTemplate = "ahe_z10";
    encodeVehicleSeat(out, EvEnterVehicle, 13, 10, s);
    encodeVehicleSeat(out, EvExitVehicle, 14, 10, s);
    encodeVehicleDestroyed(out, 15, 10, 1819, 29328, "ahe_z10", -1, pos);
    ControlPointState c;
    encodeControlPoint(out, 16, 10, 1, c);
    ChatEvent chat;
    chat.sender = -1; chat.channel = 12; chat.channelName = "ServerMessage"; chat.text = "rdy";
    encodeChat(out, 17, 10, chat);
    ProjectileState pr;
    encodeProjectileFired(out, 18, 10, pr);
    ProjectileHitEvent h;
    encodeProjectileHit(out, 19, 10, h);
    encodeKick(out, 20, 10, 3, "x");
    encodeBan(out, 21, 10, 3, "x", 1, 0);
    encodeBot(out, 22, 10, 255, BotCreated, 1, 2);
    WeaponFireEvent fire;
    fire.shooter = 255; fire.rootNetId = 1819; fire.weapon = "ahe_z10_30mm"; fire.projectile = "ahe_z10_30mm_projectile";
    fire.rounds = 2;
    encodeWeaponFire(out, 23, 10, fire);
    std::vector<WeaponSlot> slots(2);
    slots[0].slot = 0; slots[0].templateName = "ahe_z10_30mm";
    slots[1].slot = 1; slots[1].templateName = "ahe_z10_FlareLauncher";
    encodeWeapons(out, 24, 10, 255, slots);
    ProjectileCollisionEvent col;
    col.id = 1852; col.templateName = "agm114_hellfire_tv"; col.shooter = 254; col.kind = kCollisionObject;
    col.flags = kCollisionPartIsSoldier | kCollisionRewound; col.rootNetId = 1799; col.rootObjectId = 29312;
    col.rootTemplate = "ahe_ah1z"; col.partTemplate = "us_light_soldier"; col.partPlayer = 3; col.material = 7;
    col.latency = 0.3f;
    encodeProjectileCollision(out, 25, 10, col);
    ProjectileTraceEvent tr;
    tr.id = 1852; tr.templateName = "agm114_hellfire_tv"; tr.shooter = -1; tr.firstTick = 77; tr.flags = kTraceCut;
    for (int k = 0; k < 9; ++k)   // one more than kMaxTraceTargets: the ninth is not written
    {
        TraceTarget t;
        t.netId = 1800 + k; t.templateName = "v";
        tr.targets.push_back(t);
    }
    tr.samples.resize(kMaxTraceSamples + 5);   // the last 5 are not written
    tr.poses.resize(tr.samples.size() * tr.targets.size());
    tr.samples[1].lag = 0.15f;
    tr.poses[1 * 9 + 7].testedPos[0] = 1.0f; tr.poses[1 * 9 + 7].testedPos[1] = 2.0f;   // sample 1, target 7 (the last written)
    tr.poses[1 * 9 + 7].testedPos[2] = 3.0f;
    encodeProjectileTrace(out, 26, 10, tr);
    const std::vector<proto::Frame> f = frames(out);
    CHECK(f.size() == 26);
    for (size_t i = 0; i < f.size(); ++i)
    {
        Reader r(&f[i].payload[0], f[i].payload.size());
        CHECK(r.u32() == i + 1 && r.u32() == 10);
        CHECK(f[i].type >= 0x1201 && f[i].type <= 0x121A);
    }
    // Event types in table order.
    const uint16_t expected[26] = { EvGameState, EvRoundStart, EvRoundEnd, EvPlayerConnect, EvPlayerDisconnect, EvSpawn,
                                    EvDeath, EvRevive, EvTeamChange, EvScoreChange, EvKill, EvDamage, EvEnterVehicle,
                                    EvExitVehicle, EvVehicleDestroyed, EvControlPoint, EvChat, EvProjectileFired,
                                    EvProjectileHit, EvKick, EvBan, EvBot, EvWeaponFire, EvWeapons,
                                    EvProjectileCollision, EvProjectileTrace };
    for (size_t i = 0; i < f.size() && i < 26; ++i)
        CHECK(f[i].type == expected[i]);
    {
        Reader r(&f[24].payload[8], f[24].payload.size() - 8);   // ProjectileCollision
        std::string sv;
        CHECK(r.u32() == 1852);
        r.str(sv);
        CHECK(sv == "agm114_hellfire_tv" && r.i16() == 254 && r.u8() == kCollisionObject && r.u8() == 6);
        CHECK(r.u32() == 1799 && r.u32() == 29312);
        r.str(sv);
        CHECK(sv == "ahe_ah1z");
        r.str(sv);
        CHECK(sv == "us_light_soldier" && r.i16() == 3);
        float v[3];
        for (int k = 0; k < 3; ++k)
            readVec3(r, v);
        CHECK(r.u16() == 7 && r.f32() == 0.3f);
        for (int k = 0; k < 4; ++k)
            readVec3(r, v);
        CHECK(r.done());
    }
    {
        Reader r(&f[25].payload[8], f[25].payload.size() - 8);   // ProjectileTrace
        std::string sv;
        CHECK(r.u32() == 1852);
        r.str(sv);
        CHECK(sv == "agm114_hellfire_tv" && r.i16() == -1 && r.u32() == 77 && r.u8() == kTraceCut);
        CHECK(r.u8() == kMaxTraceTargets);
        for (size_t k = 0; k < kMaxTraceTargets; ++k)
        {
            CHECK(r.u32() == 1800 + k);
            r.str(sv);
        }
        CHECK(r.u16() == kMaxTraceSamples);
        float v[3];
        for (size_t i = 0; i < kMaxTraceSamples; ++i)
        {
            readVec3(r, v);
            readVec3(r, v);
            const float lag = r.f32();
            if (i == 1)
                CHECK(lag == 0.15f);
            for (size_t k = 0; k < kMaxTraceTargets; ++k)
            {
                readVec3(r, v);
                readVec3(r, v);
                readVec3(r, v);
                if (i == 1 && k == 7)
                    CHECK(v[0] == 1.0f && v[1] == 2.0f && v[2] == 3.0f);
                readVec3(r, v);
            }
        }
        CHECK(r.done());
        CHECK(f[25].payload.size() < kMaxOutboundPayload);
    }
    {
        Reader r(&f[22].payload[8], f[22].payload.size() - 8);   // WeaponFire
        std::string sv;
        CHECK(r.i16() == 255 && r.u32() == 1819);
        r.str(sv);
        CHECK(sv == "ahe_z10_30mm");
        r.str(sv);
        CHECK(sv == "ahe_z10_30mm_projectile" && r.u8() == 2);
        float v[3];
        readVec3(r, v);
        readVec3(r, v);
        CHECK(r.done());
    }
    {
        Reader r(&f[23].payload[8], f[23].payload.size() - 8);   // Weapons
        std::string sv;
        CHECK(r.u8() == 255 && r.u8() == 2 && r.u8() == 0);
        r.str(sv);
        CHECK(sv == "ahe_z10_30mm" && r.u8() == 1);
        r.str(sv);
        CHECK(sv == "ahe_z10_FlareLauncher" && r.done());
    }
    // Field checks for the denser ones.
    {
        Reader r(&f[10].payload[8], f[10].payload.size() - 8);   // Kill
        std::string sv;
        CHECK(r.u8() == 3 && r.i16() == 255 && r.u8() == KillNormal);
        r.str(sv);
        CHECK(sv == "ahe_z10_S8Launcher" && r.u8() == 2 && r.u32() == 1800);
        r.str(sv);
        float v[3];
        readVec3(r, v);
        CHECK(r.u8() == 1 && r.u32() == 1819);
        r.str(sv);
        readVec3(r, v);
        CHECK(r.u8() == 1 && r.u8() == 254 && r.i32() == 2 && r.done());
    }
    {
        Reader r(&f[4].payload[8], f[4].payload.size() - 8);   // PlayerDisconnect
        CHECK(r.u8() == 3 && r.u8() == DiscKicked && r.u16() == 21 && r.done());
    }
    {
        Reader r(&f[7].payload[8], f[7].payload.size() - 8);   // Revive with no medic
        CHECK(r.u8() == 3 && r.i16() == -1 && r.done());
    }
    {
        Reader r(&f[16].payload[8], f[16].payload.size() - 8);   // Chat
        std::string sv;
        CHECK(r.i16() == -1 && r.u8() == 0 && r.u8() == 12);
        r.str(sv);
        CHECK(sv == "ServerMessage" && r.u32() == 0 && r.u8() == 0);
        r.str(sv);
        CHECK(sv == "rdy" && r.done());
    }
}

void testReplies()
{
    std::vector<uint8_t> out;
    encodeOk(out, 9);
    std::vector<std::string> lines;
    lines.push_back("a");
    lines.push_back("");
    lines.push_back("\xC2\xA7");
    encodeConsoleResult(out, 10, true, lines);
    std::vector<ObjectInfo> objects(2);
    objects[0].objectId = 5; objects[0].templateName = "fence";
    encodeObjectList(out, 11, true, objects);
    encodeObjectSpawned(out, 12, 300, 0);
    encodePing(out, MsgPing, 13, 0x0102030405060708ull);
    encodePing(out, MsgPong, 13, 0x0102030405060708ull);
    encodeError(out, 14, ErrDisabled, "off");
    const std::vector<proto::Frame> f = frames(out);
    CHECK(f.size() == 7);
    CHECK(f[0].type == MsgOk && f[0].payload.size() == 4);
    {
        Reader r(&f[1].payload[0], f[1].payload.size());
        std::string s;
        CHECK(r.u32() == 10 && r.u8() == 1 && r.u16() == 3);
        r.str(s);
        CHECK(s == "a");
        r.str(s);
        CHECK(s.empty());
        r.str(s);
        CHECK(s == "\xC2\xA7" && r.done());
    }
    {
        Reader r(&f[2].payload[0], f[2].payload.size());
        std::string s;
        CHECK(r.u32() == 11 && r.u8() == 1 && r.u16() == 2 && r.u32() == 5 && r.u32() == 0);
        r.str(s);
        CHECK(s == "fence");
    }
    CHECK(f[3].type == MsgObjectSpawned && f[3].payload.size() == 12);
    {
        Reader r(&f[4].payload[0], f[4].payload.size());
        CHECK(f[4].type == MsgPing && r.u32() == 13 && r.u64() == 0x0102030405060708ull && r.done());
    }
    CHECK(f[5].type == MsgPong);
    {
        Reader r(&f[6].payload[0], f[6].payload.size());
        std::string s;
        CHECK(f[6].type == MsgError && r.u32() == 14 && r.u16() == ErrDisabled);
        r.str(s);
        CHECK(s == "off" && r.done());
    }
}

// ---------------------------------------------------------------- command parsing

struct Case
{
    uint16_t type;
    std::vector<uint8_t> payload;
};

// One valid payload for every WebAdmin message.
std::vector<Case> validCommands()
{
    std::vector<Case> cs;
    std::vector<uint8_t> p;
    Writer w(p);
#define CASE(t) do { Case c; c.type = (t); c.payload = p; cs.push_back(c); p.clear(); } while (0)
    w.u32(1); CASE(MsgRequestSnapshot);
    w.u32(2); w.u8(StreamPlayers); w.u8(30); w.u8(0); CASE(MsgSubscribe);
    w.u32(3); w.u8(StreamProjectiles); w.u8(5); w.u8(1); w.str("agm114_hellfire_tv"); CASE(MsgSubscribe);
    w.u32(4); w.u8(StreamDamageEvents); w.u8(0); w.u8(0); CASE(MsgSubscribe);
    w.u32(5); w.u8(StreamVehicles); CASE(MsgUnsubscribe);
    w.u32(6); w.str("sv.serverName"); CASE(CmdConsole);
    w.u32(7); w.u8(2); w.u8(3); w.str("\xC2\xA7" "C1001hi"); CASE(CmdSay);
    w.u32(8); w.u8(RefPlayer); w.u32(3); w.u8(3); for (int i = 0; i < 6; ++i) w.f32(1.0f); CASE(CmdTeleport);
    w.u32(9); w.u8(RefNetId); w.u32(1819); w.u8(1); w.f32(0.5f); CASE(CmdSetHealth);
    w.u32(10); w.u8(3); w.u8(1); CASE(CmdKillPlayer);
    w.u32(11); w.u8(3); w.u8(2); w.u8(1); CASE(CmdSetTeam);
    w.u32(12); w.u8(3); w.u8(0x1F); for (int i = 0; i < 5; ++i) w.i32(-i); CASE(CmdSetScore);
    w.u32(13); w.u8(3); w.u8(21); w.u8(1); CASE(CmdRank);
    w.u32(14); w.u8(3); w.i32(2051907); w.i32(1); CASE(CmdMedal);
    w.u32(15); w.u8(3); w.u8(0); w.i32(0); CASE(CmdGameEvent);
    w.u32(16); w.u8(3); w.u8(78); w.i32(-1); CASE(CmdHudEvent);
    w.u32(17); w.u8(3); w.str(""); CASE(CmdKick);
    w.u32(18); w.u8(3); w.u8(1); w.u8(2); w.u32(3600); w.str("tk"); CASE(CmdBan);
    w.u32(19); w.u8(4); w.u16(0); w.str("dalian_2_v_2"); w.str("gpm_cq"); w.u16(16); CASE(CmdMap);
    w.u32(20); w.str("fence_wood"); w.u16(100); CASE(CmdListObjects);
    w.u32(21); w.u8(RefConsoleId); w.u32(29328); CASE(CmdSetObjectActive);
    w.u32(22); w.str("ahe_ah1z"); for (int i = 0; i < 6; ++i) w.f32(2.0f); w.u8(1); CASE(CmdSpawnObject);
    w.u32(23); w.u8(RefNetId); w.u32(5); CASE(CmdDeleteObject);
    w.u32(24); w.u64(99); CASE(MsgPing);
    w.u32(25); w.u64(99); CASE(MsgPong);
    w.u32(26); w.u16(6); w.str("x"); CASE(MsgError);
    w.u32(27); w.u8(StreamAim); w.u8(10); w.u8(0); CASE(MsgSubscribe);
    w.u32(28); w.u8(StreamWeaponFireEvents); w.u8(0); w.u8(0); CASE(MsgSubscribe);
    w.u32(29); w.u8(StreamWeaponFireEvents); w.u8(0); w.u8(1); w.str("ahe_z10_FlareLauncher"); CASE(MsgSubscribe);
    w.u32(30); w.u8(StreamWeaponEvents); w.u8(0); w.u8(0); CASE(MsgSubscribe);
    w.u32(31); w.u8(StreamProjectileReplay); w.u8(0); w.u8(1); w.str("agm114_hellfire_tv"); CASE(MsgSubscribe);
    w.u32(32); w.str("Enable mod X?"); w.u16(30); w.u8(0); w.u8(50); w.u8(1); CASE(CmdStartVote);
    w.u32(33); CASE(CmdCancelVote);
    w.u32(34); w.i16(-1); w.u8(43); w.u8(3); w.u8(1); w.u8(2); w.u8(3); CASE(CmdHudEventRaw);
    w.u32(35); w.i16(3); w.f32(60.0f); CASE(CmdRoundClock);
    w.u32(36); w.u8(2); w.u8(1); w.u8(7); w.u8(1); w.u32(1819); for (int i = 0; i < 9; ++i) w.f32(1.0f); CASE(CmdSquadOrder);
    w.u32(37); w.u32(7); w.u8(1); w.u8(0); w.str("nasa"); w.str("{\"altitude\":300}"); w.str("script.onTick(() => {});"); CASE(CmdStartScript);
    w.u32(38); w.u32(7); CASE(CmdStopScript);
    w.u32(39); w.u32(7); w.str("{\"altitude\":250}"); CASE(CmdScriptMessage);
    w.u32(40); CASE(CmdListScripts);
    w.u32(41); w.u8(3); w.u8(1); CASE(CmdSeatPreference);
    w.u32(42); w.u8(3); w.u8(0); CASE(CmdSeatPreference);
    w.u32(43); w.u8(255); w.u8(255); CASE(CmdSeatPreference);
    w.u32(44); w.u8(1); w.u32(3); w.u8(0); CASE(CmdSetCollision);
    w.u32(45); w.u8(2); w.u32(1819); w.u8(1); CASE(CmdSetCollision);
    w.u32(46); w.u8(3); w.u32(29328); w.u8(0); CASE(CmdSetCollision);
    w.u32(47); w.u8(StreamServerHealth); w.u8(0); w.u8(0); CASE(MsgSubscribe);
#undef CASE
    return cs;
}

void testCommandParsing()
{
    const std::vector<Case> cs = validCommands();
    for (size_t i = 0; i < cs.size(); ++i)
    {
        const Case& c = cs[i];
        Command cmd;
        std::string bad;
        const bool ok = parseCommand(c.type, &c.payload[0], c.payload.size(), cmd, &bad);
        CHECK(ok && bad.empty());
        if (!ok)
            std::printf("  type 0x%04X: %s\n", c.type, bad.c_str());
        CHECK(cmd.requestId == i + 1);
        CHECK(isWebAdminMessage(c.type));
        // Every shorter payload is malformed (never a bad value), and one extra byte too.
        for (size_t n = 0; n < c.payload.size(); ++n)
        {
            Command t;
            std::string b;
            CHECK(!parseCommand(c.type, n ? &c.payload[0] : 0, n, t, &b));
        }
        std::vector<uint8_t> longer(c.payload);
        longer.push_back(0);
        Command t;
        std::string b;
        CHECK(!parseCommand(c.type, &longer[0], longer.size(), t, &b) && b.empty());
    }
    // Spot checks of parsed values and conversions.
    {
        Command cmd;
        CHECK(parseCommand(cs[6].type, &cs[6].payload[0], cs[6].payload.size(), cmd, 0));
        CHECK(cmd.target == 2 && cmd.player == 3 && cmd.text == "\xA7" "C1001hi");   // UTF-8 -> 1252
        CHECK(parseCommand(cs[11].type, &cs[11].payload[0], cs[11].payload.size(), cmd, 0));
        CHECK(cmd.flags == 0x1F && cmd.ints[4] == -4);
        CHECK(parseCommand(cs[17].type, &cs[17].payload[0], cs[17].payload.size(), cmd, 0));
        CHECK(cmd.target == 1 && cmd.value8 == 2 && cmd.seconds == 3600 && cmd.text == "tk");
        CHECK(parseCommand(cs[23].type, &cs[23].payload[0], cs[23].payload.size(), cmd, 0));
        CHECK(cmd.clock == 99);
        CHECK(parseCommand(cs[40].type, &cs[40].payload[0], cs[40].payload.size(), cmd, 0));
        CHECK(cmd.type == CmdSeatPreference && cmd.requestId == 41 && cmd.player == 3 && cmd.value8 == 1);
        CHECK(parseCommand(cs[42].type, &cs[42].payload[0], cs[42].payload.size(), cmd, 0));
        CHECK(cmd.requestId == 43 && cmd.player == 255 && cmd.value8 == 255);
        CHECK(parseCommand(cs[43].type, &cs[43].payload[0], cs[43].payload.size(), cmd, 0));
        CHECK(cmd.type == CmdSetCollision && cmd.requestId == 44 && cmd.ref.kind == RefPlayer && cmd.ref.id == 3 && cmd.flags == 0);
        CHECK(parseCommand(cs[44].type, &cs[44].payload[0], cs[44].payload.size(), cmd, 0));
        CHECK(cmd.requestId == 45 && cmd.ref.kind == RefNetId && cmd.ref.id == 1819 && cmd.flags == 1);
        CHECK(parseCommand(cs[45].type, &cs[45].payload[0], cs[45].payload.size(), cmd, 0));
        CHECK(cmd.requestId == 46 && cmd.ref.kind == RefConsoleId && cmd.ref.id == 29328 && cmd.flags == 0);
    }
    // Bad values: well-formed, so Error 7 for that request, not a framing error.
    struct Bad { uint16_t type; std::vector<uint8_t> payload; };
    std::vector<Bad> bads;
    std::vector<uint8_t> p;
    Writer w(p);
#define BAD(t) do { Bad b; b.type = (t); b.payload = p; bads.push_back(b); p.clear(); } while (0)
    w.u32(1); w.u8(9); w.u8(30); w.u8(0); BAD(MsgSubscribe);                                  // unknown stream
    w.u32(1); w.u8(StreamPlayers); w.u8(0); w.u8(0); BAD(MsgSubscribe);                       // rate 0
    w.u32(1); w.u8(StreamPlayers); w.u8(31); w.u8(0); BAD(MsgSubscribe);                      // rate 31
    w.u32(1); w.u8(StreamProjectiles); w.u8(5); w.u8(0); BAD(MsgSubscribe);                   // no filter
    w.u32(1); w.u8(StreamPlayers); w.u8(5); w.u8(1); w.str("x"); BAD(MsgSubscribe);           // filter where none fits
    w.u32(1); w.u8(StreamAim); w.u8(0); w.u8(0); BAD(MsgSubscribe);                           // aim rate 0
    w.u32(1); w.u8(StreamAim); w.u8(5); w.u8(1); w.str("x"); BAD(MsgSubscribe);               // aim takes no filter
    w.u32(1); w.u8(StreamWeaponEvents); w.u8(0); w.u8(1); w.str("x"); BAD(MsgSubscribe);      // weapons takes none
    w.u32(1); w.u8(StreamProjectileReplay); w.u8(0); w.u8(0); BAD(MsgSubscribe);              // replay needs a filter
    w.u32(1); w.u8(22); w.u8(0); w.u8(0); BAD(MsgSubscribe);                                  // unknown event stream
    w.u32(1); w.u8(StreamServerHealth); w.u8(0); w.u8(1); w.str("x"); BAD(MsgSubscribe);      // health takes no filter
    w.u32(1); w.str(""); BAD(CmdConsole);
    w.u32(1); w.str("a\nb"); BAD(CmdConsole);
    w.u32(1); w.str(std::string(4097, 'a')); BAD(CmdConsole);
    w.u32(1); w.str("\xC0\xAF"); BAD(CmdConsole);                                             // overlong UTF-8
    w.u32(1); w.u8(3); w.u8(0); w.str("x"); BAD(CmdSay);                                       // target 3
    w.u32(1); w.u8(1); w.u8(3); w.str("x"); BAD(CmdSay);                                       // team 3
    w.u32(1); w.u8(0); w.u8(0); w.str(""); BAD(CmdSay);
    w.u32(1); w.u8(4); w.u32(3); w.u8(1); for (int i = 0; i < 6; ++i) w.f32(0); BAD(CmdTeleport);   // kind 4
    w.u32(1); w.u8(1); w.u32(3); w.u8(0); for (int i = 0; i < 6; ++i) w.f32(0); BAD(CmdTeleport);   // no flags
    w.u32(1); w.u8(1); w.u32(3); w.u8(1); w.f32(std::numeric_limits<float>::quiet_NaN()); for (int i = 0; i < 5; ++i) w.f32(0); BAD(CmdTeleport);
    w.u32(1); w.u8(1); w.u32(3); w.u8(1); w.f32(9000); for (int i = 0; i < 5; ++i) w.f32(0); BAD(CmdTeleport);
    w.u32(1); w.u8(1); w.u32(3); w.u8(2); for (int i = 0; i < 5; ++i) w.f32(0); w.f32(std::numeric_limits<float>::infinity()); BAD(CmdTeleport);
    w.u32(1); w.u8(1); w.u32(3); w.u8(1); w.f32(1.5f); BAD(CmdSetHealth);                      // fraction > 1
    w.u32(1); w.u8(1); w.u32(3); w.u8(0); w.f32(-1.0f); BAD(CmdSetHealth);
    w.u32(1); w.u8(3); w.u8(2); BAD(CmdKillPlayer);                                            // unknown flag
    w.u32(1); w.u8(3); w.u8(3); w.u8(0); BAD(CmdSetTeam);                                      // team 3
    w.u32(1); w.u8(3); w.u8(0); for (int i = 0; i < 5; ++i) w.i32(0); BAD(CmdSetScore);        // empty mask
    w.u32(1); w.u8(3); w.u8(22); w.u8(0); BAD(CmdRank);
    w.u32(1); w.u8(3); w.u8(25); w.i32(0); BAD(CmdGameEvent);
    w.u32(1); w.u8(3); w.u8(79); w.i32(0); BAD(CmdHudEvent);
    w.u32(1); w.u8(3); w.str("a\x01"); BAD(CmdKick);
    w.u32(1); w.u8(3); w.u8(2); w.u8(0); w.u32(0); w.str(""); BAD(CmdBan);                    // by 2
    w.u32(1); w.u8(3); w.u8(0); w.u8(2); w.u32(0); w.str(""); BAD(CmdBan);                    // 0 seconds
    w.u32(1); w.u8(0); w.u16(0); w.str(""); w.str(""); w.u16(0); BAD(CmdMap);                 // action 0
    w.u32(1); w.u8(4); w.u16(0); w.str("dalian"); w.str(""); w.u16(0); BAD(CmdMap);           // no mode
    w.u32(1); w.str("x"); w.u16(0); BAD(CmdListObjects);
    w.u32(1); w.str("x"); w.u16(4097); BAD(CmdListObjects);
    w.u32(1); w.u8(RefPlayer); w.u32(3); BAD(CmdSetObjectActive);
    w.u32(1); w.u8(RefPlayer); w.u32(3); BAD(CmdDeleteObject);
    w.u32(1); w.str("x"); w.f32(0); w.f32(20000); w.f32(0); for (int i = 0; i < 3; ++i) w.f32(0); w.u8(0); BAD(CmdSpawnObject);
    w.u32(1); w.str(""); for (int i = 0; i < 6; ++i) w.f32(0); w.u8(0); BAD(CmdSpawnObject);
    w.u32(1); w.str("q"); w.u16(4); w.u8(0); w.u8(50); w.u8(1); BAD(CmdStartVote);              // 4 s
    w.u32(1); w.str("q"); w.u16(601); w.u8(0); w.u8(50); w.u8(1); BAD(CmdStartVote);            // 601 s
    w.u32(1); w.str("q"); w.u16(30); w.u8(3); w.u8(50); w.u8(1); BAD(CmdStartVote);             // team 3
    w.u32(1); w.str("q"); w.u16(30); w.u8(0); w.u8(0); w.u8(1); BAD(CmdStartVote);              // 0 %
    w.u32(1); w.str("q"); w.u16(30); w.u8(0); w.u8(101); w.u8(1); BAD(CmdStartVote);            // 101 %
    w.u32(1); w.str("q"); w.u16(30); w.u8(0); w.u8(50); w.u8(65); BAD(CmdStartVote);            // 65 ballots
    w.u32(1); w.str(""); w.u16(30); w.u8(0); w.u8(50); w.u8(1); BAD(CmdStartVote);              // no question
    w.u32(1); w.str(std::string(97, 'q')); w.u16(30); w.u8(0); w.u8(50); w.u8(1); BAD(CmdStartVote);
    w.u32(1); w.str("a\x01"); w.u16(30); w.u8(0); w.u8(50); w.u8(1); BAD(CmdStartVote);         // control byte
    w.u32(1); w.i16(-1); w.u8(73); w.u8(0); BAD(CmdHudEventRaw);                                // frees its payload
    w.u32(1); w.i16(-1); w.u8(38); w.u8(0); BAD(CmdHudEventRaw);                                // a pointer payload
    w.u32(1); w.i16(-2); w.u8(66); w.u8(0); BAD(CmdHudEventRaw);
    w.u32(1); w.i16(256); w.u8(66); w.u8(0); BAD(CmdHudEventRaw);
    w.u32(1); w.i16(-1); w.u8(66); w.u8(53); for (int i = 0; i < 53; ++i) w.u8(0); BAD(CmdHudEventRaw);   // 53 bytes
    w.u32(1); w.i16(-1); w.f32(-1.0f); BAD(CmdRoundClock);
    w.u32(1); w.i16(-1); w.f32(std::numeric_limits<float>::quiet_NaN()); BAD(CmdRoundClock);
    w.u32(1); w.i16(-1); w.f32(86401.0f); BAD(CmdRoundClock);
    w.u32(1); w.u8(3); w.u8(1); w.u8(5); w.u8(0); w.u32(0); for (int i = 0; i < 9; ++i) w.f32(0); BAD(CmdSquadOrder);   // team 3
    w.u32(1); w.u8(1); w.u8(0); w.u8(5); w.u8(0); w.u32(0); for (int i = 0; i < 9; ++i) w.f32(0); BAD(CmdSquadOrder);   // squad 0
    w.u32(1); w.u8(1); w.u8(1); w.u8(5); w.u8(2); w.u32(0); for (int i = 0; i < 9; ++i) w.f32(0); BAD(CmdSquadOrder);   // flags
    w.u32(1); w.u8(1); w.u8(1); w.u8(5); w.u8(0); w.u32(65536); for (int i = 0; i < 9; ++i) w.f32(0); BAD(CmdSquadOrder);
    w.u32(1); w.u8(1); w.u8(1); w.u8(5); w.u8(0); w.u32(0); w.f32(std::numeric_limits<float>::infinity()); for (int i = 0; i < 8; ++i) w.f32(0); BAD(CmdSquadOrder);
    w.u32(1); w.u32(0); w.u8(1); w.u8(0); w.str("n"); w.str(""); w.str("1"); BAD(CmdStartScript);             // instance 0
    w.u32(1); w.u32(7); w.u8(0); w.u8(0); w.str("n"); w.str(""); w.str("1"); BAD(CmdStartScript);             // API 0
    w.u32(1); w.u32(7); w.u8(1); w.u8(1); w.str("n"); w.str(""); w.str("1"); BAD(CmdStartScript);             // flags
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str(""); w.str(""); w.str("1"); BAD(CmdStartScript);              // no name
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str(std::string(65, 'n')); w.str(""); w.str("1"); BAD(CmdStartScript);
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("a\tb"); w.str(""); w.str("1"); BAD(CmdStartScript);          // control byte
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("n\xC3\xA5"); w.str(""); w.str("1"); BAD(CmdStartScript);     // not ASCII
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("n"); w.str("\xFF"); w.str("1"); BAD(CmdStartScript);         // arguments not UTF-8
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("n"); w.str(""); w.str(""); BAD(CmdStartScript);              // no source
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("n"); w.str(""); w.str(std::string(60001, ' ')); BAD(CmdStartScript);
    w.u32(1); w.u32(7); w.u8(1); w.u8(0); w.str("n"); w.str(""); w.str("1;\xC0\x80"); BAD(CmdStartScript);    // source not UTF-8
    w.u32(1); w.u32(7); w.str(std::string(8193, '1')); BAD(CmdScriptMessage);
    w.u32(1); w.u32(7); w.str("\xED\xA0\x80"); BAD(CmdScriptMessage);                                       // a surrogate
    w.u32(1); w.u8(3); w.u8(2); BAD(CmdSeatPreference);                                                    // seat 2
    w.u32(1); w.u8(3); w.u8(254); BAD(CmdSeatPreference);
    w.u32(1); w.u8(1); w.u32(3); w.u8(2); BAD(CmdSetCollision);                                          // flag bit 1
    w.u32(1); w.u8(1); w.u32(3); w.u8(0x81); BAD(CmdSetCollision);
    w.u32(1); w.u8(0); w.u32(3); w.u8(1); BAD(CmdSetCollision);                                          // kind 0
    w.u32(1); w.u8(4); w.u32(3); w.u8(1); BAD(CmdSetCollision);                                          // kind 4
#undef BAD
    for (size_t i = 0; i < bads.size(); ++i)
    {
        Command cmd;
        std::string bad;
        const bool ok = parseCommand(bads[i].type, &bads[i].payload[0], bads[i].payload.size(), cmd, &bad);
        CHECK(!ok && !bad.empty());
        if (ok || bad.empty())
            std::printf("  bad case %u (type 0x%04X) was not a value error\n", static_cast<unsigned>(i), bads[i].type);
    }
    // Module-only and unknown types are not WebAdmin messages.
    const uint16_t notInbound[] = { MsgHello, MsgAuth, MsgWelcome, MsgSnapshot, MsgPlayerStream, EvKill, MsgOk,
                                    MsgConsoleResult, 0x0001, 0x0101, 0x1999 };
    for (size_t i = 0; i < sizeof(notInbound) / sizeof(notInbound[0]); ++i)
        CHECK(!isWebAdminMessage(notInbound[i]));
}

// ---------------------------------------------------------------- session (handshake)

struct Handshake
{
    AdminLink link;
    AdminSession session;
    uint8_t nm[32], nw[32];
    Handshake(const std::vector<uint8_t>& key)
        : link(1024 * 1024), session(&link, 1, kServerId, key, "test")
    {
        for (int i = 0; i < 32; ++i) { nm[i] = static_cast<uint8_t>(i + 1); nw[i] = static_cast<uint8_t>(200 - i); }
        session.setNonce(nm);
    }
    std::vector<uint8_t> challenge(const std::vector<uint8_t>& key)
    {
        uint8_t proof[32];
        hmacSha256(key, proofTranscript(kWebAdminProofLabel, kServerId, nm, nw), proof);
        std::vector<uint8_t> p(nw, nw + 32);
        p.insert(p.end(), proof, proof + 32);
        return frame(MsgChallenge, p);
    }
};

void testHandshake()
{
    const std::vector<uint8_t> key = key16();
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        CHECK(h.session.start(out));
        std::vector<proto::Frame> f = frames(out);
        CHECK(f.size() == 1 && f[0].type == MsgHello);
        out.clear();
        const std::vector<uint8_t> c = h.challenge(key);
        CHECK(h.session.onBytes(&c[0], c.size(), out) && h.session.authenticated());
        f = frames(out);
        CHECK(f.size() == 1 && f[0].type == MsgAuth && f[0].payload.size() == 32);
        uint8_t expected[32];
        hmacSha256(key, proofTranscript(kServerProofLabel, kServerId, h.nm, h.nw), expected);
        CHECK(f.size() == 1 && std::memcmp(&f[0].payload[0], expected, 32) == 0);
        std::deque<Command> cmds;
        h.link.drainCommands(cmds);
        CHECK(cmds.size() == 1 && cmds[0].type == Command::LinkUp && cmds[0].session == 1);

        // Commands after the handshake reach the link with the session number; a bad value
        // becomes an in-order Error 7 request; Pong is consumed.
        const std::vector<Case> cs = validCommands();
        std::vector<uint8_t> all;
        for (size_t i = 0; i < cs.size(); ++i)
        {
            const std::vector<uint8_t> fr = frame(cs[i].type, cs[i].payload);
            all.insert(all.end(), fr.begin(), fr.end());
        }
        std::vector<uint8_t> bad;
        Writer w(bad);
        w.u32(77); w.u8(3); w.u8(9); w.u8(0);
        const std::vector<uint8_t> badFrame = frame(CmdSetTeam, bad);
        all.insert(all.end(), badFrame.begin(), badFrame.end());
        out.clear();
        // Byte by byte: framing must not depend on read sizes.
        for (size_t i = 0; i < all.size(); ++i)
            CHECK(h.session.onBytes(&all[i], 1, out));
        CHECK(out.empty());
        cmds.clear();
        h.link.drainCommands(cmds);
        // Every valid case except Pong, plus the bad one; Error 6 from WebAdmin is passed on.
        CHECK(cmds.size() == cs.size() - 1 + 1);
        bool sawInvalid = false;
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            CHECK(cmds[i].session == 1);
            CHECK(cmds[i].type != MsgPong);
            if (!cmds[i].invalid.empty())
                sawInvalid = cmds[i].requestId == 77 && cmds[i].type == CmdSetTeam;
        }
        CHECK(sawInvalid);

        // A second Challenge is malformed: Error 2 and close.
        out.clear();
        CHECK(!h.session.onBytes(&c[0], c.size(), out));
        f = frames(out);
        CHECK(f.size() == 1 && f[0].type == MsgError);
    }
    // Wrong key: the module refuses WebAdmin's proof with Error 8 and sends no Auth.
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        h.session.start(out);
        out.clear();
        std::vector<uint8_t> other = key;
        other[0] ^= 1;
        const std::vector<uint8_t> c = h.challenge(other);
        CHECK(!h.session.onBytes(&c[0], c.size(), out) && !h.session.authenticated());
        std::vector<proto::Frame> f = frames(out);
        CHECK(f.size() == 1 && f[0].type == MsgError);
        if (f.size() == 1)
        {
            Reader r(&f[0].payload[0], f[0].payload.size());
            CHECK(r.u32() == 0 && r.u16() == ErrAuth);
        }
        std::deque<Command> cmds;
        h.link.drainCommands(cmds);
        CHECK(cmds.empty());
    }
    // Before authentication: a command is Error 2, an unknown type Error 3, WebAdmin's Error closes.
    {
        const uint16_t types[] = { CmdConsole, 0x7777, MsgError };
        const uint16_t codes[] = { ErrMalformed, ErrUnknownType, 0 };
        for (int i = 0; i < 3; ++i)
        {
            Handshake h(key);
            std::vector<uint8_t> out;
            h.session.start(out);
            out.clear();
            std::vector<uint8_t> p;
            Writer w(p);
            w.u32(1);
            if (types[i] == MsgError)
            {
                w.u16(ErrAuth);
                w.str("unknown server id");
            }
            else
                w.str("x");
            const std::vector<uint8_t> fr = frame(types[i], p);
            CHECK(!h.session.onBytes(&fr[0], fr.size(), out));
            std::vector<proto::Frame> f = frames(out);
            if (codes[i])
            {
                CHECK(f.size() == 1 && f[0].type == MsgError);
                if (f.size() == 1)
                {
                    Reader r(&f[0].payload[0], f[0].payload.size());
                    r.u32();
                    CHECK(r.u16() == codes[i]);
                }
            }
            else
                CHECK(f.empty() && h.session.peerErrorCode() == ErrAuth);
        }
    }
    // Framing: oversize, nonzero reserved, a malformed Challenge.
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        h.session.start(out);
        out.clear();
        uint8_t big[8] = { 0x01, 0x00, 0x01, 0x00, 0x02, 0x10, 0, 0 };   // 65537 bytes
        CHECK(!h.session.onBytes(big, 8, out));
    }
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        h.session.start(out);
        out.clear();
        const std::vector<uint8_t> fr = frame(MsgChallenge, std::vector<uint8_t>(64, 0), 1);
        CHECK(!h.session.onBytes(&fr[0], fr.size(), out));
    }
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        h.session.start(out);
        out.clear();
        const std::vector<uint8_t> fr = frame(MsgChallenge, std::vector<uint8_t>(63, 0));
        CHECK(!h.session.onBytes(&fr[0], fr.size(), out));
    }
}

// Random and mutated input never crashes a session, before or after authentication.
void testFuzz()
{
    const std::vector<uint8_t> key = key16();
    std::mt19937 rng(1234);
    const std::vector<Case> cs = validCommands();
    int closes = 0;
    for (int round = 0; round < 3000; ++round)
    {
        Handshake h(key);
        std::vector<uint8_t> out;
        h.session.start(out);
        if (round % 2)
        {
            const std::vector<uint8_t> c = h.challenge(key);
            h.session.onBytes(&c[0], c.size(), out);
        }
        bool open = true;
        for (int k = 0; k < 20 && open; ++k)
        {
            std::vector<uint8_t> bytes;
            if (rng() % 3 == 0)
            {
                bytes.resize(rng() % 200);
                for (size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<uint8_t>(rng());
            }
            else
            {
                const Case& c = cs[rng() % cs.size()];
                std::vector<uint8_t> p = c.payload;
                if (!p.empty() && rng() % 2)
                    p[rng() % p.size()] = static_cast<uint8_t>(rng());
                if (!p.empty() && rng() % 4 == 0)
                    p.resize(rng() % p.size());
                bytes = frame(c.type, p);
            }
            out.clear();
            open = h.session.onBytes(bytes.empty() ? 0 : &bytes[0], bytes.size(), out);
        }
        if (!open)
            ++closes;
        std::deque<Command> cmds;
        h.link.drainCommands(cmds);
        CHECK(cmds.size() <= AdminLink::kMaxCommands);
    }
    CHECK(closes > 0);
}

// ---------------------------------------------------------------- the link

void testLink()
{
    AdminLink link(64 * 1024);
    std::vector<uint8_t> f(1000, 1);
    // Nothing is queued without an open session.
    link.sendReply(1, f);
    CHECK(!link.sendEvent(1, f));
    std::vector<uint8_t> out;
    CHECK(!link.takeOutbound(out));

    link.openSession(1);
    std::vector<uint8_t> a(10, 0xA), b(10, 0xB), s(5, 0x5), s2(5, 0x6), g(3, 0x3);
    link.sendReply(1, a);
    CHECK(link.sendEvent(1, b));
    link.sendLatest(1, AdminLink::SlotPlayers, s);
    link.sendLatest(1, AdminLink::SlotPlayers, s2);   // replaces the unsent one
    link.sendLatest(1, AdminLink::SlotGame, g);
    std::vector<uint8_t> wrong(7, 0x7);
    link.sendReply(2, wrong);                          // another session: dropped
    CHECK(link.takeOutbound(out));
    CHECK(out.size() == 10 + 10 + 5 + 3 && out[0] == 0xA && out[10] == 0xB && out[20] == 0x6 && out[25] == 0x3);

    // Events beyond the queue size are dropped and counted; replies are not, until four times
    // the size marks the link overloaded.
    int queued = 0;
    for (int i = 0; i < 200; ++i)
    {
        std::vector<uint8_t> e(1000, 0xE);
        if (link.sendEvent(1, e))
            ++queued;
    }
    CHECK(queued == 65 && link.takeDroppedEvents() == 135 && link.takeDroppedEvents() == 0);
    CHECK(!link.overloaded());
    for (int i = 0; i < 300; ++i)
    {
        std::vector<uint8_t> r(1000, 0xF);
        link.sendReply(1, r);
    }
    CHECK(link.overloaded());
    out.clear();
    CHECK(link.takeOutbound(out) && out.size() == 365 * 1000);
    link.closeSession();
    CHECK(!link.overloaded());

    // The command queue is bounded; link changes always fit.
    for (size_t i = 0; i < AdminLink::kMaxCommands; ++i)
    {
        Command c;
        c.type = CmdKick;
        CHECK(link.pushCommand(c));
    }
    Command extra;
    extra.type = CmdKick;
    CHECK(!link.pushCommand(extra));
    Command down;
    down.type = Command::LinkDown;
    CHECK(link.pushCommand(down));
    std::deque<Command> cmds;
    link.drainCommands(cmds);
    CHECK(cmds.size() == AdminLink::kMaxCommands + 1);
}

// ---------------------------------------------------------------- configuration

void testConfig()
{
    const std::string key(64, 'a'), pin(64, 'b');
    {
        Config c;
        std::vector<std::string> d;
        CHECK(parseConfig("adminConnect 127.0.0.1:16710\nadminServerId 127.0.0.1:16573\nadminKey " + key +
                              "\nadminCertSha256 " + pin + "\n",
                          c, d));
        CHECK(c.adminEnabled && c.adminHost == "127.0.0.1" && c.adminPort == 16710 && c.adminTls &&
              c.adminKey.size() == 32 && c.adminCertSha256.size() == 32 && c.adminIdentity() && c.adminAllowConsole &&
              c.adminAllowObjects && c.adminAllowScripts && c.adminQueueBytes == 4 * 1024 * 1024);
    }
    {
        Config c;
        std::vector<std::string> d;
        CHECK(parseConfig("adminTls 0\nadminConnect localhost:16710\nadminServerId x\nadminKey " + key +
                              "\nadminAllowConsole 0\nadminAllowObjects 0\nadminAllowScripts 0\nadminQueueBytes 65536\n",
                          c, d));
        CHECK(!c.adminTls && !c.adminIdentity() && !c.adminAllowConsole && !c.adminAllowObjects && !c.adminAllowScripts &&
              c.adminQueueBytes == 65536);
        Config bad;
        std::vector<std::string> diag;
        CHECK(!parseConfig("adminTls 0\nadminConnect localhost:16710\nadminServerId x\nadminKey " + key +
                               "\nadminAllowScripts 2\n", bad, diag) && !diag.empty());
    }
    {
        Config c;
        std::vector<std::string> d;
        CHECK(parseConfig("adminTls 0\nadminSendIdentity 1\nadminConnect 127.0.0.1:1\nadminServerId x\nadminKey " + key, c, d));
        CHECK(c.adminIdentity());
    }
    const char* bad[] = {
        "adminConnect 127.0.0.1:16567\nadminServerId x\nadminKey %K\nadminCertSha256 %P",
        "adminConnect 127.0.0.1\nadminServerId x\nadminKey %K\nadminCertSha256 %P",
        "adminConnect bad_host!:1\nadminServerId x\nadminKey %K\nadminCertSha256 %P",
        "adminConnect 127.0.0.1:1\nadminKey %K\nadminCertSha256 %P",                       // no server id
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminCertSha256 %P",                   // no key
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminKey %K",                          // no pin with TLS
        "adminConnect 10.0.0.1:1\nadminServerId x\nadminKey %K\nadminTls 0",               // clear text off loopback
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminKey abcd\nadminCertSha256 %P",    // short key
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminKey %K\nadminCertSha256 abcd",    // short pin
        "adminConnect 127.0.0.1:1\nadminServerId a b\nadminKey %K\nadminCertSha256 %P",
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminKey %K\nadminCertSha256 %P\nadminTls 2",
        "adminConnect 127.0.0.1:1\nadminServerId x\nadminKey %K\nadminCertSha256 %P\nadminQueueBytes 100",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        std::string text = bad[i];
        size_t at;
        while ((at = text.find("%K")) != std::string::npos)
            text.replace(at, 2, key);
        while ((at = text.find("%P")) != std::string::npos)
            text.replace(at, 2, pin);
        Config c;
        std::vector<std::string> d;
        const bool ok = parseConfig(text, c, d);
        CHECK(!ok);
        if (ok)
            std::printf("  bad config %u accepted\n", static_cast<unsigned>(i));
    }
    // Admin keys without adminConnect: a warning, not an error; the channel stays off.
    {
        Config c;
        std::vector<std::string> d;
        CHECK(parseConfig("adminKey " + key, c, d) && !c.adminEnabled && !d.empty());
    }
}

// ---------------------------------------------------------------- the I/O thread against TLS

// A minimal WebAdmin: accepts connections on 127.0.0.1, optionally wraps them in TLS with a fresh
// self-signed P-256 certificate, answers Hello with a Challenge, checks Auth and then echoes a
// Pong to a Ping.
class TestServer
{
public:
    TestServer(bool tls, const std::vector<uint8_t>& key)
        : m_tls(tls), m_key(key), m_listen(-1), m_port(0), m_stop(false), m_authOk(0), m_handshakes(0), m_ctx(0),
          m_pkey(0), m_cert(0)
    {
        if (tls)
            makeCertificate();
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&a), &len);
        m_port = ntohs(a.sin_port);
        ::listen(m_listen, 4);
        m_thread = std::thread(&TestServer::run, this);
    }

    ~TestServer() { stop(); }

    void stop()
    {
        if (!m_thread.joinable())
            return;
        m_stop = true;
        ::shutdown(m_listen, SHUT_RDWR);
        ::close(m_listen);
        m_thread.join();
        if (m_ctx) SSL_CTX_free(m_ctx);
        if (m_cert) X509_free(m_cert);
        if (m_pkey) EVP_PKEY_free(m_pkey);
        m_ctx = 0; m_cert = 0; m_pkey = 0;
    }

    int port() const { return m_port; }
    std::vector<uint8_t> pin() const { return m_pin; }
    int authenticated() const { return m_authOk.load(); }
    int handshakes() const { return m_handshakes.load(); }

private:
    void makeCertificate()
    {
        EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, 0);
        EVP_PKEY_keygen_init(kc);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(kc, &m_pkey);
        EVP_PKEY_CTX_free(kc);
        m_cert = X509_new();
        X509_set_version(m_cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(m_cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(m_cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(m_cert), 3600);
        X509_set_pubkey(m_cert, m_pkey);
        X509_NAME* name = X509_get_subject_name(m_cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("test webadmin"), -1, -1, 0);
        X509_set_issuer_name(m_cert, name);
        X509_sign(m_cert, m_pkey, EVP_sha256());
        X509_PUBKEY* pk = X509_get_X509_PUBKEY(m_cert);
        const int len = i2d_X509_PUBKEY(pk, 0);
        std::vector<uint8_t> der(static_cast<size_t>(len));
        unsigned char* p = &der[0];
        i2d_X509_PUBKEY(pk, &p);
        m_pin.resize(32);
        sha256(&der[0], der.size(), &m_pin[0]);
        m_ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION);
        SSL_CTX_use_certificate(m_ctx, m_cert);
        SSL_CTX_use_PrivateKey(m_ctx, m_pkey);
    }

    // Blocking I/O over either a plain socket or an SSL.
    bool readAll(int fd, SSL* ssl, uint8_t* buf, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            const int r = ssl ? SSL_read(ssl, buf + got, static_cast<int>(n - got))
                              : static_cast<int>(::recv(fd, buf + got, n - got, 0));
            if (r <= 0)
                return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    bool writeAll(int fd, SSL* ssl, const std::vector<uint8_t>& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const int r = ssl ? SSL_write(ssl, &data[sent], static_cast<int>(data.size() - sent))
                              : static_cast<int>(::send(fd, &data[sent], data.size() - sent, MSG_NOSIGNAL));
            if (r <= 0)
                return false;
            sent += static_cast<size_t>(r);
        }
        return true;
    }

    bool readFrame(int fd, SSL* ssl, proto::Frame& f)
    {
        uint8_t h[8];
        if (!readAll(fd, ssl, h, 8))
            return false;
        const uint32_t len = h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<uint32_t>(h[3]) << 24);
        f.type = static_cast<uint16_t>(h[4] | (h[5] << 8));
        f.payload.resize(len);
        return len == 0 || readAll(fd, ssl, &f.payload[0], len);
    }

    void serve(int fd)
    {
        timeval tv = { 5, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        SSL* ssl = 0;
        if (m_tls)
        {
            ssl = SSL_new(m_ctx);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1)
            {
                ERR_clear_error();
                SSL_free(ssl);
                ::close(fd);
                return;
            }
        }
        ++m_handshakes;
        proto::Frame hello;
        if (readFrame(fd, ssl, hello) && hello.type == MsgHello)
        {
            Reader r(&hello.payload[0], hello.payload.size());
            r.u16();
            std::string id;
            r.str(id);
            uint8_t nm[32], nw[32], proof[32];
            r.bytes(nm, 32);
            randomBytes(nw, 32);
            hmacSha256(m_key, proofTranscript(kWebAdminProofLabel, id, nm, nw), proof);
            std::vector<uint8_t> p(nw, nw + 32);
            p.insert(p.end(), proof, proof + 32);
            proto::Frame auth;
            if (writeAll(fd, ssl, frame(MsgChallenge, p)) && readFrame(fd, ssl, auth) && auth.type == MsgAuth &&
                auth.payload.size() == 32)
            {
                uint8_t expected[32];
                hmacSha256(m_key, proofTranscript(kServerProofLabel, id, nm, nw), expected);
                if (std::memcmp(expected, &auth.payload[0], 32) == 0)
                {
                    ++m_authOk;
                    // Serve until the client goes or the test stops: answer Pings.
                    proto::Frame f;
                    while (!m_stop && readFrame(fd, ssl, f))
                    {
                        if (f.type == MsgPing)
                        {
                            Reader pr(&f.payload[0], f.payload.size());
                            std::vector<uint8_t> out;
                            const uint32_t req = pr.u32();
                            encodePing(out, MsgPong, req, pr.u64());
                            writeAll(fd, ssl, out);
                        }
                    }
                }
            }
        }
        if (ssl)
            SSL_free(ssl);
        ::close(fd);
    }

    void run()
    {
        while (!m_stop)
        {
            const int fd = ::accept(m_listen, 0, 0);
            if (fd < 0)
                continue;
            serve(fd);
        }
    }

    bool m_tls;
    std::vector<uint8_t> m_key;
    int m_listen;
    int m_port;
    std::atomic<bool> m_stop;
    std::atomic<int> m_authOk;
    std::atomic<int> m_handshakes;
    std::vector<uint8_t> m_pin;
    SSL_CTX* m_ctx;
    EVP_PKEY* m_pkey;
    X509* m_cert;
    std::thread m_thread;
};

std::vector<std::string> g_ioLog;
std::mutex g_ioLogMutex;

void ioLog(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_ioLogMutex);
    g_ioLog.push_back(line);
}

bool waitFor(const std::function<bool()>& cond, int ms)
{
    for (int i = 0; i < ms / 10; ++i)
    {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

AdminIoConfig ioConfig(int port, bool tls, const std::vector<uint8_t>& key, const std::vector<uint8_t>& pin)
{
    AdminIoConfig c;
    c.host = "127.0.0.1";
    c.port = port;
    c.serverId = kServerId;
    c.key = key;
    c.tls = tls;
    c.pin = pin;
    c.build = "test";
    c.initialBackoffMs = 100;
    c.maxBackoffMs = 400;
    c.handshakeTimeoutMs = 3000;
    return c;
}

bool sawLinkUp(AdminLink& link)
{
    std::deque<Command> cmds;
    link.drainCommands(cmds);
    for (size_t i = 0; i < cmds.size(); ++i)
        if (cmds[i].type == Command::LinkUp)
            return true;
    return false;
}

void testIo()
{
    const std::vector<uint8_t> key = key16();
    // TLS with the right pin: the link authenticates and is up.
    {
        TestServer server(true, key);
        AdminLink link(1024 * 1024);
        AdminIo io(ioConfig(server.port(), true, key, server.pin()), &link, ioLog);
        std::string error;
        CHECK(io.start(error));
        CHECK(waitFor([&] { return io.linksAuthenticated() == 1; }, 5000));
        CHECK(waitFor([&] { return server.authenticated() == 1; }, 2000));
        CHECK(sawLinkUp(link));
        CHECK(io.lastPeerPin() == toHex(&server.pin()[0], 32));
        // Frames queued by the main thread reach the server; the link stays up.
        std::vector<uint8_t> ping;
        encodePing(ping, MsgPing, 1, 2);
        link.sendReply(1, ping);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(io.linksAuthenticated() == 1);
        io.stop();
    }
    // TLS with a wrong pin: the module aborts the handshake and never sends Hello.
    {
        TestServer server(true, key);
        AdminLink link(1024 * 1024);
        std::vector<uint8_t> wrong = server.pin();
        wrong[5] ^= 0x40;
        AdminIo io(ioConfig(server.port(), true, key, wrong), &link, ioLog);
        std::string error;
        CHECK(io.start(error));
        CHECK(waitFor([&] { return io.attempts() >= 2; }, 5000));
        CHECK(io.linksAuthenticated() == 0 && server.handshakes() == 0 && server.authenticated() == 0);
        CHECK(io.lastCloseReason().find("does not match adminCertSha256") != std::string::npos);
        CHECK(!sawLinkUp(link));
        io.stop();
    }
    // Wrong key over TLS: the module refuses WebAdmin's proof (Error 8 path).
    {
        TestServer server(true, key);
        AdminLink link(1024 * 1024);
        std::vector<uint8_t> other = key;
        other[3] ^= 1;
        AdminIo io(ioConfig(server.port(), true, other, server.pin()), &link, ioLog);
        std::string error;
        CHECK(io.start(error));
        CHECK(waitFor([&] { return io.attempts() >= 2 && server.handshakes() >= 1; }, 5000));
        CHECK(io.linksAuthenticated() == 0 && server.authenticated() == 0);
        CHECK(io.lastCloseReason().find("proof") != std::string::npos);
        io.stop();
    }
    // Plain loopback, then a reconnect after the server restarts on the same port.
    {
        AdminLink link(1024 * 1024);
        int port = 0;
        {
            TestServer server(false, key);
            port = server.port();
            AdminIo io(ioConfig(port, false, key, std::vector<uint8_t>()), &link, ioLog);
            std::string error;
            CHECK(io.start(error));
            CHECK(waitFor([&] { return io.linksAuthenticated() == 1; }, 5000));
            server.stop();
            CHECK(waitFor([&] { return io.attempts() >= 3; }, 5000));   // backing off against a closed port
            io.stop();
        }
        CHECK(sawLinkUp(link));
    }
}

// ---------------------------------------------------------------- server health

void testHealth()
{
    using namespace serverext::health;

    // quantile: the smallest value with at least q of the values at or below it
    {
        std::vector<uint32_t> none;
        CHECK(quantile(none, 0.5) == 0);
        std::vector<uint32_t> one(1, 7);
        CHECK(quantile(one, 0.5) == 7 && quantile(one, 0.99) == 7 && quantile(one, 1.0) == 7);
        std::vector<uint32_t> v;
        for (uint32_t i = 100; i >= 1; --i)
            v.push_back(i);
        CHECK(quantile(v, 0.5) == 50 && quantile(v, 0.99) == 99 && quantile(v, 1.0) == 100 && quantile(v, 0.001) == 1);
        CHECK(quantile(v, 0.0) == 1 && quantile(v, 2.0) == 100);
    }
    CHECK(share(0, 0) == 0 && share(5, 0) == 0 && share(1, 4) == 2500 && share(9, 4) == 10000 && share(1, 3) == 3333);
    CHECK(share(278, 10000) == 278 && share(0xFFFFFFFFFFFFull, 0xFFFFFFFFFFFFull) == 10000);

    // A steady 30 Hz server: the first call sets the starting point, the window completes on the 5th interval.
    {
        Sampler s(5);
        uint64_t wall = 1000000, cpu = 500;
        CHECK(!s.add(wall, cpu, 0));
        for (int i = 1; i <= 4; ++i)
            CHECK(!s.add(wall += 33333, cpu += 300, 80));
        CHECK(s.add(wall += 33333, cpu += 300, 80));
        const Window& w = s.window();
        CHECK(w.ticks == 5 && w.wallUs == 5 * 33333 && w.flags == 0);
        CHECK(w.intervalP50Us == 33333 && w.intervalP99Us == 33333 && w.intervalMaxUs == 33333);
        CHECK(w.lateTicks == 0 && w.shortTicks == 0);
        CHECK(w.cpuP50Us == 300 && w.cpuP99Us == 300 && w.cpuMaxUs == 300 && w.handlerP99Us == 80 && w.handlerMaxUs == 80);
        CHECK(w.involuntarySwitches == 0 && w.majorFaults == 0 && w.cpus == 0 && w.residentKb == 0);

        // The next window: a frame 70 ms late, then two ticks in one frame, and a level load noted in it.
        s.noteLevelLoad();
        CHECK(!s.add(wall += 33333, cpu += 300, 90));
        CHECK(!s.add(wall += 103333, cpu += 20000, 90));      // late
        CHECK(!s.add(wall += 400, cpu += 350, 2500));         // caught up in the same frame
        CHECK(!s.add(wall += 33000, cpu += 300, 90));
        CHECK(s.add(wall += 33333, cpu += 300, 90));
        const Window& x = s.window();
        CHECK(x.ticks == 5 && x.wallUs == 33333 + 103333 + 400 + 33000 + 33333 && x.flags == kFlagLevelLoad);
        CHECK(x.lateTicks == 1 && x.shortTicks == 1 && x.intervalMaxUs == 103333 && x.intervalP50Us == 33333);
        CHECK(x.cpuMaxUs == 20000 && x.cpuP50Us == 300 && x.handlerMaxUs == 2500 && x.handlerP99Us == 2500);

        // The flag belongs to that window only.
        for (int i = 0; i < 4; ++i)
            CHECK(!s.add(wall += 33333, cpu += 300, 90));
        CHECK(s.add(wall += 33333, cpu += 300, 90) && s.window().flags == 0 && s.window().lateTicks == 0);
    }
    // The limits of late and short, clocks that go back, and values beyond 32 bits.
    {
        Sampler s(6);
        uint64_t wall = 10, cpu = 10;
        CHECK(!s.add(wall, cpu, 0));
        CHECK(!s.add(wall += kLateIntervalUs, cpu, 0));         // exactly the limit: not late
        CHECK(!s.add(wall += kLateIntervalUs + 1, cpu, 0));     // late
        CHECK(!s.add(wall += kShortIntervalUs, cpu, 0));        // exactly the limit: not short
        CHECK(!s.add(wall += kShortIntervalUs - 1, cpu, 0));    // short
        CHECK(!s.add(wall - 5, cpu - 5, 0xFFFFFFFFu));          // both clocks went back: no time, counted as short
        CHECK(s.add(wall + 0x200000000ull, cpu + 0x200000000ull, 0));
        const Window& w = s.window();
        CHECK(w.ticks == 6 && w.lateTicks == 2 && w.shortTicks == 2);
        CHECK(w.intervalMaxUs == 0xFFFFFFFFu && w.cpuMaxUs == 0xFFFFFFFFu && w.wallUs == 0xFFFFFFFFu);
        CHECK(w.handlerMaxUs == 0xFFFFFFFFu);
    }
    // A window size outside 1..65535 is brought inside it.
    {
        Sampler s(0);
        CHECK(!s.add(0, 0, 0) && s.add(33333, 100, 1) && s.window().ticks == 1);
    }

    // /proc/stat
    {
        const std::string stat =
            "cpu  1000 20 300 80000 40 5 15 2600 7 9\n"
            "cpu0 500 10 150 40000 20 2 8 1300 0 0\n"
            "cpu1 500 10 150 40000 20 3 7 1300 0 0\n"
            "intr 12345 0 0\nctxt 99\nbtime 1\nprocesses 5\n";
        CpuTimes c;
        CHECK(parseProcStat(stat.data(), stat.size(), c));
        CHECK(c.busy == 1000 + 20 + 300 + 5 + 15 && c.steal == 2600 && c.ioWait == 40 && c.cpus == 2);
        CHECK(c.total == c.busy + 80000 + 40 + 2600);
        // Hostile and broken text: nothing is read past the size, and a bad file leaves the output alone.
        CpuTimes keep;
        keep.busy = 42;
        const char* const bad[] = {
            "", "cpu", "cpu ", "cpu  1 2 3 4 5 6 7", "cpu  1 2 3 4 5 6 7 x", "cpu  -1 2 3 4 5 6 7 8",
            "cpu0 1 2 3 4 5 6 7 8\n", "cpux 1 2 3 4 5 6 7 8", "intr 1 2 3 4 5 6 7 8 9\n",
            "cpu  99999999999999999999999 2 3 4 5 6 7 8", "cpu  1 2 3 4 5 6 7\n8", "\n\n\n" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
            CHECK(!parseProcStat(bad[i], std::strlen(bad[i]), keep) && keep.busy == 42);
        for (size_t cut = 0; cut < stat.size(); ++cut)
        {
            CpuTimes part;
            parseProcStat(stat.data(), cut, part);      // any truncation: must not crash
        }
        CHECK(!parseProcStat(stat.data(), 20, keep));   // the cpu line cut before its eighth field
        // No line end after the last field, tabs, and no cpuN lines.
        const std::string bare = "cpu\t1 2 3 4 5 6 7 8";
        CHECK(!parseProcStat(bare.data(), bare.size(), c));   // "cpu" must be followed by a space
        const std::string spaced = "cpu \t 1 2 3 4 5 6 7 8";
        CHECK(parseProcStat(spaced.data(), spaced.size(), c) && c.cpus == 0 && c.steal == 8 && c.total == 1 + 2 + 3 + 6 + 7 + 4 + 5 + 8);
    }
    // /proc/self/statm
    {
        uint64_t pages = 0;
        const std::string statm = "40000 12345 900 5 0 20000 0\n";
        CHECK(parseStatm(statm.data(), statm.size(), pages) && pages == 12345);
        uint64_t keep = 7;
        const char* const bad[] = { "", "40000", "40000 ", "40000 x", "x 1", "40000\n12345", "-1 5" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
            CHECK(!parseStatm(bad[i], std::strlen(bad[i]), keep) && keep == 7);
    }

    // The event: 8 bytes of sequence number and tick, then 63 bytes.
    {
        Window w;
        w.ticks = 300; w.wallUs = 0x01020304; w.flags = kFlagHostValid | kFlagLevelLoad; w.intervalMaxUs = 70000;
        w.lateTicks = 2; w.shortTicks = 3; w.hostSteal = 278; w.residentKb = 0xA0B0C0D0;
        std::vector<uint8_t> buf;
        encodeServerHealth(buf, 9, 1234, w);
        const std::vector<proto::Frame> f = frames(buf);
        CHECK(f.size() == 1 && f[0].type == EvServerHealth && f[0].payload.size() == 8 + 63);
        if (f.size() == 1 && f[0].payload.size() == 8 + 63)
        {
            Reader r(&f[0].payload[0], f[0].payload.size());
            CHECK(r.u32() == 9 && r.u32() == 1234 && r.u16() == 300 && r.u32() == 0x01020304 && r.u8() == 6);
            CHECK(r.u32() == 0 && r.u32() == 0 && r.u32() == 70000 && r.u16() == 2 && r.u16() == 3);
            for (int i = 0; i < 7; ++i)
                CHECK(r.u32() == 0);
            CHECK(r.u16() == 0 && r.u16() == 0 && r.u16() == 278 && r.u16() == 0 && r.u32() == 0xA0B0C0D0 && r.done());
        }
    }

    // The monitor on this thread: real clocks, getrusage and the host thread reading this machine's /proc.
    {
        static Monitor m;   // its thread lives for the process
        CHECK(m.start(false));
        timespec wait = { 1, 300000000 };
        nanosleep(&wait, 0);        // one host sample before the first tick, so the first window has a base
        unsigned int events = 0, logs = 0;
        volatile unsigned int spin = 0;
        for (unsigned int i = 0; i <= kEventWindowTicks; ++i)
        {
            const unsigned int done = m.tick(123);
            events += (done & Monitor::EventWindow) ? 1 : 0;
            logs += (done & Monitor::LogWindow) ? 1 : 0;
            m.noteLevelLoad();
            for (unsigned int k = 0; k < 20000; ++k)
                spin += k;
            timespec nap = { 0, 5000000 };
            nanosleep(&nap, 0);     // 300 ticks of 5 ms: the window spans a host sample
        }
        const Window& w = m.eventWindow();
        CHECK(events == 1 && logs == 0 && w.ticks == kEventWindowTicks);
        CHECK(w.wallUs > 1400000 && w.wallUs < 60000000 && w.intervalP50Us >= 5000 && w.intervalMaxUs >= w.intervalP99Us);
        CHECK(w.cpuMaxUs > 0 && w.cpuP50Us < 5000000 && w.handlerMaxUs == 123 && w.handlerP99Us == 123);
        CHECK((w.flags & kFlagLevelLoad) && !(w.flags & kFlagStepped));
        CHECK((w.flags & kFlagHostValid) && w.cpus >= 1 && w.residentKb > 0);
        CHECK(w.hostBusy + w.hostSteal + w.hostIoWait <= 10000);
        std::printf("  health monitor: %u ticks in %.3f s, interval us p50 %u max %u, cpu us p50 %u max %u, %u switches, "
                    "host %u CPUs busy %.2f%% steal %.2f%%, resident %u KiB\n", w.ticks, w.wallUs / 1e6, w.intervalP50Us,
                    w.intervalMaxUs, w.cpuP50Us, w.cpuMaxUs, w.involuntarySwitches, w.cpus, w.hostBusy / 100.0,
                    w.hostSteal / 100.0, w.residentKb);
    }
}

// ---------------------------------------------------------------- golden fixtures

// The canonical samples of tools/adminclient/adminclient.py golden_samples(). The module's messages
// must encode to the fixture bytes; WebAdmin's must parse to the sample values.
void set3(float v[3], float a, float b, float c) { v[0] = a; v[1] = b; v[2] = c; }

GameState goldenGame()
{
    GameState g;
    g.status = 1; g.beginRound = 3; g.timeToNext = 12.5f; g.tickets[0] = 150; g.tickets[1] = 0; g.roundTime = 61.0f;
    g.timeLimit = 1800.0f; g.winner = 2; g.victoryType = 3;
    return g;
}

ControlPointState goldenCp()
{
    ControlPointState c;
    c.objectId = 400; c.netId = 0; c.templateName = "cpname_dalian_harbour"; c.team = 2; c.flagTeam = 2;
    c.takeOver = 0.5f; c.capturing = 1;
    set3(c.pos, 1, 2, 3);
    return c;
}

ProjectileState goldenProjectile()
{
    ProjectileState p;
    p.id = 0x10000 + 77; p.templateName = "agm114_hellfire_tv"; p.owner = 254; p.team = 1; p.flags = 1;
    p.targetNetId = 1820; p.timeToLive = 9.5f;
    set3(p.pos, 5, 6, 7);
    set3(p.rot, 10, 20, 30);
    set3(p.vel, 100, 0, -1);
    return p;
}

std::map<std::string, std::vector<uint8_t> > readGolden(const std::string& path)
{
    std::map<std::string, std::vector<uint8_t> > out;
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);   // a CRLF checkout
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        std::string name, type, hex;
        s >> name >> type >> hex;
        std::vector<uint8_t> bytes;
        if (parseHex(hex, bytes))
            out[name] = bytes;
    }
    return out;
}

void testGolden()
{
    const std::string path = std::string(SERVEREXT_FIXTURES) + "/admin-v1-golden.txt";
    std::map<std::string, std::vector<uint8_t> > golden = readGolden(path);
    CHECK(golden.size() == 86);
    if (golden.empty())
    {
        std::printf("  no golden fixtures at %s\n", path.c_str());
        return;
    }
    std::map<std::string, std::vector<uint8_t> > mine;
    uint8_t nonce[32], proof[32];
    for (int i = 0; i < 32; ++i) { nonce[i] = static_cast<uint8_t>(i); proof[i] = static_cast<uint8_t>(0x10 + i); }
    encodeHello(mine["hello"], kServerId, nonce, "libserverext admin 1");
    encodeAuth(mine["auth"], proof);
    {
        WelcomeInfo w;
        w.session = 3; w.tick = 900; w.serverName = "serverext admin"; w.gamePort = 16573; w.gameSpyPort = 29905;
        w.maxPlayers = 16; w.capabilities = 3; w.map = "dalian_2_v_2"; w.mode = "gpm_cq"; w.mapSize = 16; w.mapIndex = 0;
        MapEntry e;
        e.name = "dalian_2_v_2"; e.mode = "gpm_cq"; e.size = 16;
        w.maps.push_back(e);
        e.name = "daqing_2_v_2";
        w.maps.push_back(e);
        w.team1 = "CH"; w.team2 = "US";
        w.game = goldenGame();
        encodeWelcome(mine["welcome"], w);
    }
    const PlayerInfo pinfo = samplePlayerInfo();
    const VehicleState vehicle = sampleVehicle();
    encodeSnapshot(mine["snapshot"], 17, 42, 1000, 33.5, goldenGame(), std::vector<PlayerInfo>(1, pinfo),
                   std::vector<VehicleState>(1, vehicle), std::vector<ControlPointState>(1, goldenCp()),
                   std::vector<ProjectileState>(1, goldenProjectile()));
    encodePlayerStream(mine["player_stream"], 5, 1.0, std::vector<uint8_t>(1, 7),
                       std::vector<PlayerState>(1, samplePlayerState()));
    encodeVehicleStream(mine["vehicle_stream"], 5, 1.0, std::vector<VehicleState>(1, vehicle));
    encodeProjectileStream(mine["projectile_stream"], 5, 1.0, std::vector<ProjectileState>(1, goldenProjectile()));
    encodeGameStream(mine["game_stream"], 5, 1.0, goldenGame());

    float p123[3], p456[3];
    set3(p123, 1, 2, 3);
    set3(p456, 4, 5, 6);
    const int32_t tickets[2] = { 100, 90 };
    encodeGameStateEvent(mine["ev_game_state"], 1, 10, goldenGame(), 3);
    encodeRoundStart(mine["ev_round_start"], 2, 10, "dalian_2_v_2", "gpm_cq", tickets);
    encodeRoundEnd(mine["ev_round_end"], 3, 10, tickets, 1, 2);
    encodePlayerConnect(mine["ev_player_connect"], 4, 10, pinfo);
    encodePlayerDisconnect(mine["ev_player_disconnect"], 5, 10, 3, 2, 21);
    encodeSpawn(mine["ev_spawn"], 6, 10, 3, 2, 1800, 5000, "us_heavy_soldier", p123, p456);
    encodeDeath(mine["ev_death"], 7, 10, 3, 2, 1800, "us_heavy_soldier", p123);
    encodeRevive(mine["ev_revive"], 8, 10, 3, -1);
    encodeTeamChange(mine["ev_team_change"], 9, 10, 255, 2, 1, 1);
    encodeScoreChange(mine["ev_score_change"], 10, 10, 3, pinfo.score);
    {
        KillEvent k;
        k.victim = 3; k.attacker = 255; k.kind = 1; k.weapon = "ahe_z10_S8Launcher"; k.victimTeam = 2;
        k.victimRootNetId = 1800; k.victimRootTemplate = "us_heavy_soldier"; set3(k.victimPos, 1, 2, 3);
        k.attackerTeam = 1; k.attackerVehicleNetId = 1819; k.attackerVehicleTemplate = "ahe_z10";
        set3(k.attackerPos, 4, 5, 6);
        k.assists.push_back(std::make_pair(static_cast<uint8_t>(254), 2));
        encodeKill(mine["ev_kill"], 11, 10, k);
    }
    {
        DamageEvent d;
        d.victimPlayer = 3; d.victimRootNetId = 1800; d.victimRootObjectId = 5000; d.victimTemplate = "us_heavy_soldier";
        d.attacker = 255; d.weapon = "w"; d.projectile = "p"; d.amount = 10; d.before = 100; d.after = 90; d.max = 100;
        set3(d.pos, 1, 2, 3);
        d.flags = 0;
        encodeDamage(mine["ev_damage"], 12, 10, d);
    }
    {
        VehicleSeatEvent s;
        s.player = 255; s.seat = 0; s.flags = 0; s.rootNetId = 1819; s.rootObjectId = 29328; s.rootTemplate = "ahe_z10";
        s.seatTemplate = "ahe_z10";
        encodeVehicleSeat(mine["ev_enter_vehicle"], EvEnterVehicle, 13, 10, s);
        encodeVehicleSeat(mine["ev_exit_vehicle"], EvExitVehicle, 14, 10, s);
    }
    encodeVehicleDestroyed(mine["ev_vehicle_destroyed"], 15, 10, 1819, 29328, "ahe_z10", -1, p123);
    encodeControlPoint(mine["ev_control_point"], 16, 10, 1, goldenCp());
    {
        ChatEvent c;
        c.sender = -1; c.team = 0; c.channel = 12; c.channelName = "ServerMessage"; c.engineFlags = 0; c.textFlags = 0;
        c.text = "rdy";
        encodeChat(mine["ev_chat"], 17, 10, c);
    }
    encodeProjectileFired(mine["ev_projectile_fired"], 18, 10, goldenProjectile());
    {
        ProjectileHitEvent h;
        h.id = 0x10000 + 77; h.templateName = "agm114_hellfire_tv"; h.owner = 254; h.flags = 3; h.firstVictimNetId = 1820;
        set3(h.pos, 5, 6, 7);
        encodeProjectileHit(mine["ev_projectile_hit"], 19, 10, h);
    }
    encodeKick(mine["ev_kick"], 20, 10, 3, "x");
    encodeBan(mine["ev_ban"], 21, 10, 3, "x", 1, 0);
    encodeBot(mine["ev_bot"], 22, 10, 255, 1, 1, 2);
    {
        std::vector<AimState> aims(1);
        aims[0].player = 7; aims[0].flags = kAimCamera | kAimWeapon;
        set3(aims[0].cameraPos, 1.5f, 101.5f, -3.0f);
        set3(aims[0].cameraRot, 90.0f, -10.0f, 0.0f);
        set3(aims[0].weaponPos, 1.5f, 99.0f, -1.0f);
        set3(aims[0].weaponRot, 85.0f, -12.5f, 2.5f);
        encodeAimStream(mine["aim_stream"], 5, 1.0, aims);
    }
    {
        WeaponFireEvent e;
        e.shooter = 255; e.rootNetId = 1819; e.weapon = "ahe_z10_30mm"; e.projectile = "ahe_z10_30mm_projectile";
        e.rounds = 3;
        set3(e.origin, 1.0f, 2.0f, 3.0f);
        set3(e.velocity, 400.0f, -5.0f, 10.0f);
        encodeWeaponFire(mine["ev_weapon_fire"], 23, 10, e);
    }
    {
        std::vector<WeaponSlot> slots(2);
        slots[0].slot = 0; slots[0].templateName = "ahe_z10_30mm";
        slots[1].slot = 1; slots[1].templateName = "ahe_z10_FlareLauncher";
        encodeWeapons(mine["ev_weapons"], 24, 10, 255, slots);
    }
    {
        ProjectileCollisionEvent e;
        e.id = 1852; e.templateName = "agm114_hellfire_tv"; e.shooter = 254; e.kind = kCollisionObject;
        e.flags = kCollisionRewound; e.rootNetId = 1799; e.rootObjectId = 29312; e.rootTemplate = "ahe_ah1z";
        e.partTemplate = "agm114_hellfiredummy"; e.partPlayer = -1;
        set3(e.pos, 210.5f, 199.25f, 112.0f);
        set3(e.normal, 1.0f, 0.0f, 0.0f);
        set3(e.speed, -30.0f, 2.5f, 120.0f);
        e.material = 56; e.latency = 0.25f;
        set3(e.partPos, 211.5f, 199.0f, 112.25f);
        set3(e.partRot, 180.0f, 2.0f, 25.5f);
        set3(e.rootPos, 206.0f, 199.5f, 110.0f);
        set3(e.rootRot, 180.0f, 2.0f, 25.5f);
        encodeProjectileCollision(mine["ev_projectile_collision"], 25, 10, e);
    }
    {
        ProjectileTraceEvent e;
        e.id = 1852; e.templateName = "agm114_hellfire_tv"; e.shooter = 254; e.firstTick = 5000; e.flags = kTraceCollided;
        TraceTarget t;
        t.netId = 1799; t.templateName = "ahe_ah1z";
        e.targets.push_back(t);
        for (int k = 0; k < 2; ++k)
        {
            TraceSample smp;
            set3(smp.pos, 100.0f + k, 200.0f, 50.0f + 4 * k);
            set3(smp.vel, 0.0f, -1.0f, 125.0f);
            smp.lag = 0.25f;
            e.samples.push_back(smp);
            TracePose q;
            set3(q.pos, 210.0f - k, 199.0f, 112.0f);
            set3(q.rot, 180.0f, 2.0f, 25.5f);
            set3(q.testedPos, 217.5f - k, 199.5f, 111.0f);
            set3(q.testedRot, 180.0f, 1.5f, 25.0f);
            e.poses.push_back(q);
        }
        encodeProjectileTrace(mine["ev_projectile_trace"], 26, 10, e);
    }
    encodeOk(mine["ok"], 9);
    {
        std::vector<std::string> lines;
        lines.push_back("a");
        lines.push_back("");
        lines.push_back("\xC2\xA7");
        encodeConsoleResult(mine["console_result"], 10, true, lines);
    }
    {
        std::vector<ObjectInfo> objects(1);
        objects[0].objectId = 5; objects[0].netId = 0; objects[0].templateName = "fence";
        set3(objects[0].pos, 1, 2, 3);
        set3(objects[0].rot, 4, 5, 6);
        objects[0].health = -1.0f; objects[0].flags = 0;
        encodeObjectList(mine["object_list"], 11, true, objects);
    }
    encodeObjectSpawned(mine["object_spawned"], 12, 300, 0);
    encodePing(mine["ping"], MsgPing, 13, 0x0102030405060708ull);
    encodePing(mine["pong"], MsgPong, 13, 0x0102030405060708ull);
    encodeError(mine["error"], 14, ErrDisabled, "off");
    encodeVoteResult(mine["ev_vote_result"], 27, 10, 27, "Enable mod \xC2\xA7X?", 1, 3, 1, 5);
    {
        std::vector<std::pair<uint32_t, std::string> > scripts;
        scripts.push_back(std::make_pair(7u, std::string("nasa")));
        scripts.push_back(std::make_pair(9u, std::string("dance")));
        encodeScriptList(mine["script_list"], 33, scripts);
    }
    encodeScriptFinished(mine["ev_script_finished"], 28, 10, 7, "nasa", 1, "{\"ticks\":90}", 90, 1234, 56);
    encodeScriptEvent(mine["ev_script_event"], 29, 10, 7, 1, "{\"player\":3,\"y\":301.5}");
    {
        health::Window h;
        h.ticks = 300; h.wallUs = 10000123; h.flags = health::kFlagHostValid;
        h.intervalP50Us = 33334; h.intervalP99Us = 34100; h.intervalMaxUs = 61234; h.lateTicks = 1; h.shortTicks = 1;
        h.cpuP50Us = 310; h.cpuP99Us = 1250; h.cpuMaxUs = 4800; h.handlerP99Us = 350; h.handlerMaxUs = 900;
        h.involuntarySwitches = 12; h.majorFaults = 0; h.cpus = 1; h.hostBusy = 412; h.hostSteal = 278;
        h.hostIoWait = 3; h.residentKb = 159744;
        encodeServerHealth(mine["ev_server_health"], 30, 10, h);
    }

    for (std::map<std::string, std::vector<uint8_t> >::iterator it = mine.begin(); it != mine.end(); ++it)
    {
        const bool same = golden.count(it->first) && golden[it->first] == it->second;
        CHECK(same);
        if (!same)
            std::printf("  golden '%s' differs:\n    mine   %s\n    golden %s\n", it->first.c_str(),
                        toHex(it->second.empty() ? 0 : &it->second[0], it->second.size()).c_str(),
                        golden.count(it->first) ? toHex(&golden[it->first][0], golden[it->first].size()).c_str() : "-");
    }

    // WebAdmin's messages parse to the sample values.
    std::map<std::string, Command> parsed;
    for (std::map<std::string, std::vector<uint8_t> >::iterator it = golden.begin(); it != golden.end(); ++it)
    {
        if (it->first.compare(0, 4, "cmd_") != 0)
            continue;
        const std::vector<proto::Frame> fr = frames(it->second);
        Command c;
        std::string bad;
        const bool ok = fr.size() == 1 && parseCommand(fr[0].type, fr[0].payload.empty() ? 0 : &fr[0].payload[0],
                                                       fr[0].payload.size(), c, &bad) && bad.empty();
        CHECK(ok);
        parsed[it->first] = c;
    }
    CHECK(parsed.size() == 38);
    Command c;
    c = parsed["cmd_set_collision_off"]; CHECK(c.type == CmdSetCollision && c.requestId == 36 && c.ref.kind == RefPlayer && c.ref.id == 3 && c.flags == 0);
    c = parsed["cmd_set_collision_on"]; CHECK(c.type == CmdSetCollision && c.requestId == 37 && c.ref.kind == RefNetId && c.ref.id == 1819 && c.flags == 1);
    c = parsed["cmd_seat_preference"]; CHECK(c.type == CmdSeatPreference && c.requestId == 34 && c.player == 3 && c.value8 == 1);
    c = parsed["cmd_seat_preference_none"]; CHECK(c.type == CmdSeatPreference && c.requestId == 35 && c.player == 3 && c.value8 == 255);
    c = parsed["cmd_start_script"]; CHECK(c.type == CmdStartScript && c.requestId == 30 && c.instance == 7 && c.value8 == 1 && c.flags == 0 && c.text == "nasa" && c.text2 == "{\"altitude\":300}" && c.source == "script.onTick(() => {});");
    c = parsed["cmd_stop_script"]; CHECK(c.type == CmdStopScript && c.requestId == 31 && c.instance == 7);
    c = parsed["cmd_script_message"]; CHECK(c.type == CmdScriptMessage && c.requestId == 32 && c.instance == 7 && c.text2 == "{\"altitude\":250}");
    c = parsed["cmd_list_scripts"]; CHECK(c.type == CmdListScripts && c.requestId == 33);
    c = parsed["cmd_hud_event_raw"]; CHECK(c.type == CmdHudEventRaw && c.requestId == 29 && c.target16 == -1 && c.value8 == 43 && c.bytes.size() == 20 && c.bytes[4] == 2 && c.bytes[19] == 0);
    c = parsed["cmd_round_clock"]; CHECK(c.type == CmdRoundClock && c.requestId == 30 && c.target16 == 3 && c.value == 90.5f);
    c = parsed["cmd_squad_order"]; CHECK(c.type == CmdSquadOrder && c.requestId == 31 && c.target == 2 && c.player == 1 && c.value8 == 8 && c.flags == 1 && c.netId == 1819 && c.points[0] == 10.0f && c.points[8] == 30.0f);
    c = parsed["cmd_start_vote"]; CHECK(c.type == CmdStartVote && c.requestId == 27 && c.text == "Enable mod X?" && c.seconds == 30 && c.target == 2 && c.value8 == 60 && c.flags == 3);
    c = parsed["cmd_cancel_vote"]; CHECK(c.type == CmdCancelVote && c.requestId == 28);
    c = parsed["cmd_request_snapshot"]; CHECK(c.type == MsgRequestSnapshot && c.requestId == 1);
    c = parsed["cmd_subscribe_fire"]; CHECK(c.requestId == 25 && c.target == 18 && c.filters.size() == 1 && c.filters[0] == "ahe_z10_FlareLauncher");
    c = parsed["cmd_subscribe_health"]; CHECK(c.requestId == 38 && c.target == StreamServerHealth && c.filters.empty());
    c = parsed["cmd_subscribe_replay"]; CHECK(c.requestId == 26 && c.target == StreamProjectileReplay && c.filters.size() == 1 && c.filters[0] == "agm114_hellfire_tv");
    c = parsed["cmd_subscribe"]; CHECK(c.requestId == 3 && c.target == 3 && c.value8 == 5 && c.filters.size() == 1 && c.filters[0] == "agm114_hellfire_tv");
    c = parsed["cmd_unsubscribe"]; CHECK(c.requestId == 5 && c.target == 2);
    c = parsed["cmd_console"]; CHECK(c.requestId == 6 && c.text == "sv.serverName");
    c = parsed["cmd_say"]; CHECK(c.requestId == 7 && c.target == 2 && c.player == 3 && c.text == "\xA7" "C1001hi");
    c = parsed["cmd_teleport"]; CHECK(c.requestId == 8 && c.ref.kind == 1 && c.ref.id == 3 && c.flags == 3 && c.pos[1] == 1.0f && c.rot[2] == 1.0f);
    c = parsed["cmd_set_health"]; CHECK(c.requestId == 9 && c.ref.kind == 2 && c.ref.id == 1819 && c.flags == 1 && c.value == 0.5f);
    c = parsed["cmd_kill_player"]; CHECK(c.requestId == 10 && c.player == 3 && c.flags == 1);
    c = parsed["cmd_set_team"]; CHECK(c.requestId == 11 && c.player == 3 && c.value8 == 2 && c.flags == 1);
    c = parsed["cmd_set_score"]; CHECK(c.requestId == 12 && c.flags == 0x1F && c.ints[0] == 0 && c.ints[4] == -4);
    c = parsed["cmd_rank"]; CHECK(c.requestId == 13 && c.value8 == 21 && c.flags == 1);
    c = parsed["cmd_medal"]; CHECK(c.requestId == 14 && c.ints[0] == 2051907 && c.ints[1] == 1);
    c = parsed["cmd_game_event"]; CHECK(c.requestId == 15 && c.value8 == 0 && c.ints[0] == 0);
    c = parsed["cmd_hud_event"]; CHECK(c.requestId == 16 && c.value8 == 78 && c.ints[0] == -1);
    c = parsed["cmd_kick"]; CHECK(c.requestId == 17 && c.player == 3 && c.text.empty());
    c = parsed["cmd_ban"]; CHECK(c.requestId == 18 && c.target == 1 && c.value8 == 2 && c.seconds == 3600 && c.text == "tk");
    c = parsed["cmd_map"]; CHECK(c.requestId == 19 && c.target == 4 && c.index == 0 && c.text == "dalian_2_v_2" && c.text2 == "gpm_cq" && c.size == 16);
    c = parsed["cmd_list_objects"]; CHECK(c.requestId == 20 && c.text == "fence_wood" && c.maxResults == 100);
    c = parsed["cmd_set_object_active"]; CHECK(c.requestId == 21 && c.ref.kind == 3 && c.ref.id == 29328);
    c = parsed["cmd_spawn_object"]; CHECK(c.requestId == 22 && c.text == "ahe_ah1z" && c.pos[0] == 2.0f && c.rot[2] == 2.0f && c.value8 == 1);
    c = parsed["cmd_delete_object"]; CHECK(c.requestId == 23 && c.ref.kind == 2 && c.ref.id == 5);
    c = parsed["cmd_ping"]; CHECK(c.type == MsgPing && c.requestId == 24 && c.clock == 99);
    {
        const std::vector<proto::Frame> fr = frames(golden["challenge"]);
        ChallengeMsg ch;
        CHECK(fr.size() == 1 && parseChallenge(&fr[0].payload[0], fr[0].payload.size(), ch) && ch.nonce[0] == 0xC0 &&
              ch.nonce[31] == 0xDF && ch.proof[0] == 0x40 && ch.proof[31] == 0x5F);
    }
}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);   // the test server writes with OpenSSL's socket BIO
    SSL_library_init();
    testText();
    testCrypto();
    testRotation();
    testStructures();
    testSessionMessages();
    testSnapshotAndStreams();
    testEvents();
    testReplies();
    testCommandParsing();
    testHandshake();
    testFuzz();
    testLink();
    testConfig();
    testHealth();
    testGolden();
    testIo();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
