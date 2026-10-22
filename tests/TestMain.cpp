// Unit tests for serverext_core: framing, parsing, the per-connection protocol rules, input
// conditioning, config and the main/I-O thread channel. No engine, no sockets.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "Channel.h"
#include "Config.h"
#include "Protocol.h"
#include "Session.h"
#include "Neutral.h"
#include "SpawnPlan.h"

using namespace serverext;
using namespace serverext::proto;

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

std::vector<uint8_t> helloPayload(uint16_t version, const std::string& name)
{
    std::vector<uint8_t> p;
    Writer w(p);
    w.u16(version);
    w.str(name);
    return p;
}

std::vector<uint8_t> setInputPayload(uint8_t id, float a, float b, float c, float d, float lookYaw = 0.0f,
                                     float lookPitch = 0.0f, uint32_t buttons = 0)
{
    std::vector<uint8_t> p;
    Writer w(p);
    w.u8(id);
    w.u32(7);
    w.f32(a); w.f32(b); w.f32(c); w.f32(d);
    w.f32(lookYaw); w.f32(lookPitch);
    w.u32(buttons);
    return p;
}

const size_t kSetInputSize = 1 + 4 + 6 * 4 + 4;

std::vector<uint8_t> teleportPayload(uint32_t requestId, uint8_t id, float x, float y, float z, float yaw)
{
    std::vector<uint8_t> p;
    Writer w(p);
    w.u32(requestId);
    w.u8(id);
    w.f32(x); w.f32(y); w.f32(z);
    w.f32(yaw);
    return p;
}

std::vector<uint8_t> spotPayload(uint32_t requestId, uint8_t id, uint32_t target)
{
    std::vector<uint8_t> p;
    Writer w(p);
    w.u32(requestId);
    w.u8(id);
    w.u32(target);
    return p;
}

std::vector<uint8_t> restartPayload(uint32_t requestId)
{
    std::vector<uint8_t> p;
    Writer w(p);
    w.u32(requestId);
    return p;
}

// Decodes the single Error frame in bytes and returns its code, or 0.
uint16_t errorCode(const std::vector<uint8_t>& bytes)
{
    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    Frame f;
    if (d.next(f) != FrameDecoder::GotFrame || f.type != MsgError)
        return 0;
    Reader r(f.payload.data(), f.payload.size());
    r.u32();
    const uint16_t code = r.u16();
    std::string msg;
    r.str(msg);
    return r.done() ? code : 0;
}

size_t drain(Channel& ch)
{
    std::deque<Command> q;
    ch.drainCommands(q);
    return q.size();
}

void testWriterReader()
{
    std::vector<uint8_t> b;
    Writer w(b);
    w.u8(0xAB); w.u16(0x1234); w.u32(0xDEADBEEF); w.f32(1.5f); w.str("hi");
    CHECK(b.size() == 1 + 2 + 4 + 4 + 2 + 2);
    CHECK(b[1] == 0x34 && b[2] == 0x12);   // little-endian
    Reader r(b.data(), b.size());
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.f32() == 1.5f);
    std::string s;
    r.str(s);
    CHECK(s == "hi");
    CHECK(r.done());
    CHECK(r.u8() == 0 && !r.ok());   // overrun is sticky

    // A string whose length runs past the end.
    std::vector<uint8_t> bad = { 0x10, 0x00, 'a' };
    Reader r2(bad.data(), bad.size());
    r2.str(s);
    CHECK(!r2.ok() && s.empty());
}

void testDecoder()
{
    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i)
    {
        std::vector<uint8_t> f = frame(MsgSetInput, setInputPayload(static_cast<uint8_t>(i), 0, 0, 0, 0));
        stream.insert(stream.end(), f.begin(), f.end());
    }
    std::vector<uint8_t> empty = frame(MsgHello, std::vector<uint8_t>());
    stream.insert(stream.end(), empty.begin(), empty.end());

    // Byte at a time.
    FrameDecoder d;
    Frame f;
    int got = 0;
    for (size_t i = 0; i < stream.size(); ++i)
    {
        d.feed(&stream[i], 1);
        while (d.next(f) == FrameDecoder::GotFrame)
        {
            if (got < 3)
                CHECK(f.type == MsgSetInput && f.payload.size() == kSetInputSize && f.payload[0] == got);
            else
                CHECK(f.type == MsgHello && f.payload.empty());
            ++got;
        }
    }
    CHECK(got == 4);
    CHECK(d.buffered() == 0);

    // All at once.
    FrameDecoder d2;
    d2.feed(stream.data(), stream.size());
    got = 0;
    while (d2.next(f) == FrameDecoder::GotFrame)
        ++got;
    CHECK(got == 4);

    // Exactly the maximum is fine; one more is not.
    std::vector<uint8_t> big = frame(0x7777, std::vector<uint8_t>(kMaxPayload, 0x55));
    FrameDecoder d3;
    d3.feed(big.data(), big.size());
    CHECK(d3.next(f) == FrameDecoder::GotFrame && f.payload.size() == kMaxPayload);

    uint8_t over[8] = { 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0, 0 };   // 65537
    FrameDecoder d4;
    d4.feed(over, sizeof(over));
    CHECK(d4.next(f) == FrameDecoder::Oversize);

    uint8_t huge[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0, 0 };
    FrameDecoder d5;
    d5.feed(huge, sizeof(huge));
    CHECK(d5.next(f) == FrameDecoder::Oversize);

    std::vector<uint8_t> res = frame(MsgHello, helloPayload(kVersion, "x"), 1);
    FrameDecoder d6;
    d6.feed(res.data(), res.size());
    CHECK(d6.next(f) == FrameDecoder::BadReserved);

    // Partial header and partial payload wait.
    FrameDecoder d7;
    d7.feed(big.data(), 5);
    CHECK(d7.next(f) == FrameDecoder::NeedMore);
    d7.feed(big.data() + 5, 100);
    CHECK(d7.next(f) == FrameDecoder::NeedMore);
}

void testParsers()
{
    HelloMsg h;
    CHECK(parseHello(helloPayload(0, "client"), h) && h.version == 0 && h.name == "client");
    std::vector<uint8_t> p = helloPayload(0, "client");
    p.push_back(0);
    CHECK(!parseHello(p, h));                       // trailing byte
    p = helloPayload(0, "client");
    p.pop_back();
    CHECK(!parseHello(p, h));                       // truncated string

    std::vector<uint8_t> cb;
    { Writer w(cb); w.u32(42); w.str("[BOT] a"); w.u8(1); }
    CreateBotMsg c;
    CHECK(parseCreateBot(cb, c) && c.requestId == 42 && c.name == "[BOT] a" && c.team == 1);

    std::vector<uint8_t> ev;
    { Writer w(ev); w.u32(5); w.u8(255); w.u32(0); w.u8(0); }
    EnterVehicleMsg e;
    CHECK(parseEnterVehicle(ev, e) && e.requestId == 5 && e.playerId == 255 && e.vehicleId == 0 && e.seat == 0);
    ev.pop_back();
    CHECK(!parseEnterVehicle(ev, e));

    std::vector<uint8_t> rb;
    { Writer w(rb); w.u32(9); w.u8(254); }
    RemoveBotMsg r;
    CHECK(parseRemoveBot(rb, r) && r.requestId == 9 && r.playerId == 254);

    SetInputMsg s;
    CHECK(parseSetInput(setInputPayload(3, 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0x80000101u), s) && s.playerId == 3 &&
          s.tick == 7 && s.axes[AxisPitch] == 0.1f && s.axes[AxisRoll] == -0.2f && s.axes[AxisCollective] == 0.3f &&
          s.axes[AxisPedals] == -0.4f && s.axes[AxisLookYaw] == 0.5f && s.axes[AxisLookPitch] == -0.6f &&
          s.buttons == 0x80000101u);
    CHECK(kSetInputSize == 33);
    CHECK(!parseSetInput(std::vector<uint8_t>(kSetInputSize - 1, 0), s));
    CHECK(!parseSetInput(std::vector<uint8_t>(kSetInputSize + 1, 0), s));
    CHECK(!parseSetInput(std::vector<uint8_t>(25, 0), s));   // a v0 SetInput is malformed in v1
    CHECK(!parseSetInput(std::vector<uint8_t>(), s));

    RestartRoundMsg rr;
    CHECK(parseRestartRound(restartPayload(0xA1B2C3D4u), rr) && rr.requestId == 0xA1B2C3D4u);
    CHECK(!parseRestartRound(std::vector<uint8_t>(3, 0), rr));
    CHECK(!parseRestartRound(std::vector<uint8_t>(5, 0), rr));
    CHECK(!parseRestartRound(std::vector<uint8_t>(), rr));

    CHECK(isClientMessage(MsgRestartRound));
    CHECK(!isClientMessage(MsgSpawnBlocked) && !isClientMessage(MsgVehicleList));

    // v2: Teleport (21 bytes) and Spot (9 bytes), exact length only.
    TeleportMsg tp;
    CHECK(parseTeleport(teleportPayload(3, 254, -214.5f, 180.0f, 172.0f, 90.0f), tp) && tp.requestId == 3 &&
          tp.playerId == 254 && tp.pos[0] == -214.5f && tp.pos[1] == 180.0f && tp.pos[2] == 172.0f && tp.yaw == 90.0f);
    CHECK(teleportPayload(0, 0, 0, 0, 0, 0).size() == 21);
    {
        std::vector<uint8_t> t = teleportPayload(3, 254, 0, 0, 0, 0);
        t.pop_back();
        CHECK(!parseTeleport(t, tp));
        t.push_back(0);
        t.push_back(0);
        CHECK(!parseTeleport(t, tp));
        CHECK(!parseTeleport(std::vector<uint8_t>(), tp));
        // Non-finite values parse; the module rejects them with Error 7.
        CHECK(parseTeleport(teleportPayload(1, 1, std::numeric_limits<float>::quiet_NaN(), 0, 0,
                                            std::numeric_limits<float>::infinity()), tp));
    }
    SpotMsg sp;
    CHECK(parseSpot(spotPayload(8, 255, 1819), sp) && sp.requestId == 8 && sp.playerId == 255 && sp.targetId == 1819);
    CHECK(spotPayload(0, 0, 0).size() == 9);
    CHECK(!parseSpot(std::vector<uint8_t>(8, 0), sp));
    CHECK(!parseSpot(std::vector<uint8_t>(10, 0), sp));
    CHECK(!parseSpot(std::vector<uint8_t>(), sp));

    CHECK(isClientMessage(MsgTeleport) && isClientMessage(MsgSpot));
    const uint16_t moduleOnly[] = { MsgProjectileList, MsgDamage, MsgKill, MsgChat, MsgRoundState };
    for (size_t i = 0; i < sizeof(moduleOnly) / sizeof(moduleOnly[0]); ++i)
        CHECK(!isClientMessage(moduleOnly[i]));
}

// The v2 module-to-sidecar events, field by field.
void testV2Encoders()
{
    // ProjectileList
    {
        std::vector<ProjectileEntry> v(1);
        ProjectileEntry& e = v[0];
        e.id = 2101;
        e.templateName = "ahe_ah1z_hellfire";
        e.owner = 254;
        e.team = 2;
        e.flags = kProjectileGuided | kProjectileOwn;
        e.targetId = 1819;
        e.pos[0] = 1; e.pos[1] = 2; e.pos[2] = 3;
        e.vel[0] = 4; e.vel[1] = 5; e.vel[2] = 6;
        e.timeToLive = 7.5f;
        std::vector<uint8_t> b;
        encodeProjectileList(b, 44, v);
        FrameDecoder d;
        d.feed(b.data(), b.size());
        Frame f;
        CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgProjectileList);
        Reader r(f.payload.data(), f.payload.size());
        CHECK(r.u32() == 44 && r.u8() == 1 && r.u32() == 2101);
        std::string name;
        r.str(name);
        CHECK(name == "ahe_ah1z_hellfire" && r.u8() == 254 && r.u8() == 2 && r.u8() == (kProjectileGuided | kProjectileOwn));
        CHECK(r.u32() == 1819);
        CHECK(r.f32() == 1 && r.f32() == 2 && r.f32() == 3 && r.f32() == 4 && r.f32() == 5 && r.f32() == 6);
        CHECK(r.f32() == 7.5f && r.done());

        // Empty, and capped at kMaxProjectiles.
        b.clear();
        encodeProjectileList(b, 1, std::vector<ProjectileEntry>());
        CHECK(b.size() == kHeaderSize + 5 && b[kHeaderSize + 4] == 0);
        std::vector<ProjectileEntry> many(200);
        b.clear();
        encodeProjectileList(b, 1, many);
        CHECK(b[kHeaderSize + 4] == kMaxProjectiles);
        CHECK(b.size() == kHeaderSize + 5 + kMaxProjectiles * (4 + 2 + 3 + 4 + 24 + 4));
    }
    // Damage
    {
        DamageEvent e;
        e.tick = 10;
        e.flags = kDamageVictimOwn | kDamageKilled;
        e.victim = 255;
        e.victimRootId = 1819;
        e.victimTemplate = "ahe_z10";
        e.attacker = 0;
        e.weaponTemplate = "";
        e.sourceTemplate = std::string(300, 's');
        e.amount = 12.5f;
        e.before = 100.0f;
        e.after = 87.5f;
        e.max = 100.0f;
        e.pos[0] = -1; e.pos[1] = 0; e.pos[2] = 1;
        std::vector<uint8_t> b;
        encodeDamage(b, e);
        FrameDecoder d;
        d.feed(b.data(), b.size());
        Frame f;
        CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgDamage);
        Reader r(f.payload.data(), f.payload.size());
        CHECK(r.u32() == 10 && r.u8() == (kDamageVictimOwn | kDamageKilled) && r.u8() == 255 && r.u32() == 1819);
        std::string s;
        r.str(s);
        CHECK(s == "ahe_z10" && r.u8() == 0);
        r.str(s);
        CHECK(s.empty());
        r.str(s);
        CHECK(s.size() == 255);
        CHECK(r.f32() == 12.5f && r.f32() == 100.0f && r.f32() == 87.5f && r.f32() == 100.0f);
        CHECK(r.f32() == -1 && r.f32() == 0 && r.f32() == 1 && r.done());
    }
    // Kill
    {
        KillEvent k;
        k.tick = 11;
        k.victim = 254;
        k.victimTeam = 2;
        k.attacker = 255;
        k.attackerTeam = 1;
        k.weaponTemplate = "chrif_type95";
        k.flags = kKillVictimOwn | kKillAttackerOwn;
        std::vector<uint8_t> b;
        encodeKill(b, k);
        FrameDecoder d;
        d.feed(b.data(), b.size());
        Frame f;
        CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgKill);
        Reader r(f.payload.data(), f.payload.size());
        CHECK(r.u32() == 11 && r.u8() == 254 && r.u8() == 2 && r.u8() == 255 && r.u8() == 1);
        std::string s;
        r.str(s);
        CHECK(s == "chrif_type95" && r.u8() == (kKillVictimOwn | kKillAttackerOwn) && r.done());
    }
    // Chat: control bytes replaced, cut at 255 bytes.
    {
        std::vector<uint8_t> b;
        encodeChat(b, 12, kChatFromPlayer, 3, 1, 0, std::string("rdy\n\x01go") + std::string(400, 'x'));
        FrameDecoder d;
        d.feed(b.data(), b.size());
        Frame f;
        CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgChat);
        Reader r(f.payload.data(), f.payload.size());
        CHECK(r.u32() == 12 && r.u8() == kChatFromPlayer && r.u8() == 3 && r.u8() == 1 && r.u8() == 0);
        std::string s;
        r.str(s);
        CHECK(r.done() && s.size() == 255 && s.substr(0, 7) == "rdy??go");
    }
    // RoundState
    {
        std::vector<uint8_t> b;
        encodeRoundState(b, 13, 1, 3, -1.0f);
        FrameDecoder d;
        d.feed(b.data(), b.size());
        Frame f;
        CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgRoundState && f.payload.size() == 10);
        Reader r(f.payload.data(), f.payload.size());
        CHECK(r.u32() == 13 && r.u8() == 1 && r.u8() == 3 && r.f32() == -1.0f && r.done());
    }
}

void testEncoders()
{
    std::vector<uint8_t> b;
    encodeWelcome(b, 30.0f, 123, "dalian_2_v_2", "gpm_cq", 30.0f, 1000.0f, kFeatureTeleport);
    FrameDecoder d;
    d.feed(b.data(), b.size());
    Frame f;
    CHECK(d.next(f) == FrameDecoder::GotFrame && f.type == MsgWelcome);
    Reader r(f.payload.data(), f.payload.size());
    CHECK(r.u16() == 2);
    CHECK(r.f32() == 30.0f);
    CHECK(r.u32() == 123);
    std::string map, mode;
    r.str(map);
    r.str(mode);
    CHECK(map == "dalian_2_v_2" && mode == "gpm_cq");
    CHECK(r.f32() == 30.0f && r.f32() == 1000.0f && r.u8() == kFeatureTeleport && r.done());

    // TickState: other players' entries are the v1 60 bytes plus view mode and weapon count.
    std::vector<StateEntry> entries(300);
    b.clear();
    encodeTickState(b, 1, 2.0, entries);
    CHECK(b.size() == kHeaderSize + 4 + 8 + 1 + 255 * 62);
    CHECK(b[kHeaderSize + 12] == 255);

    // An own bot's entry with weapons round-trips field by field.
    {
        std::vector<StateEntry> one(2);
        StateEntry& e = one[0];
        e.playerId = 255;
        e.flags = kFlagAlive | kFlagOwnBot | kFlagInVehicle | kFlagManDown | kFlagSpotted;
        e.team = 1;
        e.seat = 0;
        e.rootId = 1819;
        e.health = 0.5f;
        e.viewMode = 3;
        e.weaponCount = 2;
        e.weapons[0].slot = 0;
        e.weapons[0].templateName = "ahe_z10_S8Launcher";
        e.weapons[0].ammoInMag = 7;
        e.weapons[0].mags = 5;
        e.weapons[0].flags = kWeaponReloading;
        e.weapons[0].reloadLeft = 0.25f;
        e.weapons[0].heat = -1.0f;
        e.weapons[1].slot = 2;
        e.weapons[1].templateName = "flares";
        e.weapons[1].ammoInMag = -1;
        e.weapons[1].mags = -1;
        e.weapons[1].flags = 0;
        e.weapons[1].reloadLeft = 0.0f;
        e.weapons[1].heat = 0.5f;
        one[1].playerId = 7;
        one[1].viewMode = kViewNone;
        b.clear();
        encodeTickState(b, 9, 1.5, one);
        FrameDecoder td;
        td.feed(b.data(), b.size());
        Frame tf;
        CHECK(td.next(tf) == FrameDecoder::GotFrame && tf.type == MsgTickState);
        Reader tr(tf.payload.data(), tf.payload.size());
        tr.u32(); tr.f32(); tr.f32();   // tick, f64 time
        CHECK(tr.u8() == 2);
        CHECK(tr.u8() == 255 && tr.u8() == (kFlagAlive | kFlagOwnBot | kFlagInVehicle | kFlagManDown | kFlagSpotted));
        CHECK(tr.u8() == 1 && tr.u8() == 0 && tr.u32() == 1819);
        for (int k = 0; k < 12; ++k) tr.f32();
        CHECK(tr.f32() == 0.5f && tr.u8() == 3 && tr.u8() == 2);
        std::string name;
        CHECK(tr.u8() == 0);
        tr.str(name);
        CHECK(name == "ahe_z10_S8Launcher" && tr.u16() == 7 && tr.u16() == 5 && tr.u8() == kWeaponReloading);
        CHECK(tr.f32() == 0.25f && tr.f32() == -1.0f);
        CHECK(tr.u8() == 2);
        tr.str(name);
        CHECK(name == "flares" && tr.u16() == 0xFFFF && tr.u16() == 0xFFFF && tr.u8() == 0 && tr.f32() == 0.0f &&
              tr.f32() == 0.5f);
        CHECK(tr.u8() == 7);
        for (int k = 0; k < 3 + 4 * 0; ++k) tr.u8();
        tr.u32();
        for (int k = 0; k < 13; ++k) tr.f32();
        CHECK(tr.u8() == kViewNone && tr.u8() == 0 && tr.done());
    }
    // Entries with long weapon names are cut so the payload stays within kMaxPayload.
    {
        std::vector<StateEntry> big(255);
        for (size_t i = 0; i < big.size(); ++i)
        {
            big[i].weaponCount = 3;
            for (int k = 0; k < 3; ++k)
                big[i].weapons[k].templateName = std::string(400, 'w');
        }
        b.clear();
        encodeTickState(b, 1, 0.0, big);
        CHECK(b.size() - kHeaderSize <= kMaxPayload);
        const uint8_t n = b[kHeaderSize + 12];
        CHECK(n > 0 && n < 255);
        CHECK(b.size() - kHeaderSize == static_cast<size_t>(13 + n * (62 + 3 * (1 + 2 + 255 + 2 + 2 + 1 + 4 + 4))));
    }

    b.clear();
    encodeSpawnBlocked(b, 254, 15);
    {
        FrameDecoder sd;
        sd.feed(b.data(), b.size());
        Frame sf;
        CHECK(sd.next(sf) == FrameDecoder::GotFrame && sf.type == MsgSpawnBlocked && sf.payload.size() == 3);
        Reader sr(sf.payload.data(), sf.payload.size());
        CHECK(sr.u8() == 254 && sr.u16() == 15 && sr.done());
    }

    // VehicleList: one entry round-trips field by field.
    {
        std::vector<VehicleEntry> v(1);
        VehicleEntry& e = v[0];
        e.netId = 1819;
        e.templateName = "ahe_z10";
        e.team = 1;
        e.flags = kVehicleDestroyed;
        e.pos[0] = -218.0f; e.pos[1] = 153.5f; e.pos[2] = 159.0f;
        e.yaw = 163.5f;
        e.health = 0.0f;
        e.seats = 2;
        e.occupied = 2;
        b.clear();
        encodeVehicleList(b, 77, v);
        FrameDecoder vd;
        vd.feed(b.data(), b.size());
        Frame vf;
        CHECK(vd.next(vf) == FrameDecoder::GotFrame && vf.type == MsgVehicleList);
        Reader vr(vf.payload.data(), vf.payload.size());
        CHECK(vr.u32() == 77 && vr.u16() == 1 && vr.u32() == 1819);
        std::string name;
        vr.str(name);
        CHECK(name == "ahe_z10" && vr.u8() == 1 && vr.u8() == kVehicleDestroyed);
        CHECK(vr.f32() == -218.0f && vr.f32() == 153.5f && vr.f32() == 159.0f && vr.f32() == 163.5f && vr.f32() == 0.0f);
        CHECK(vr.u8() == 2 && vr.u32() == 2 && vr.done());
    }
    // Empty list; and a list too large for one frame is cut at kMaxPayload, never beyond.
    {
        b.clear();
        encodeVehicleList(b, 1, std::vector<VehicleEntry>());
        CHECK(b.size() == kHeaderSize + 6);
        std::vector<VehicleEntry> many(3000);
        for (size_t i = 0; i < many.size(); ++i)
        {
            std::memset(many[i].pos, 0, sizeof(many[i].pos));
            many[i].netId = static_cast<uint32_t>(i);
            many[i].templateName = std::string(i % 2 ? 300 : 10, 'v');   // 300 is truncated to 255
            many[i].team = many[i].flags = many[i].seats = 0;
            many[i].yaw = many[i].health = 0.0f;
            many[i].occupied = 0;
        }
        b.clear();
        encodeVehicleList(b, 2, many);
        CHECK(b.size() - kHeaderSize <= kMaxPayload);
        FrameDecoder md;
        md.feed(b.data(), b.size());
        Frame mf;
        CHECK(md.next(mf) == FrameDecoder::GotFrame);
        Reader mr(mf.payload.data(), mf.payload.size());
        mr.u32();
        const uint16_t n = mr.u16();
        CHECK(n > 0 && n < many.size());
        for (uint16_t i = 0; i < n; ++i)
        {
            CHECK(mr.u32() == i);
            std::string name;
            mr.str(name);
            CHECK(name.size() == (i % 2 ? 255u : 10u));
            mr.u8(); mr.u8(); mr.f32(); mr.f32(); mr.f32(); mr.f32(); mr.f32(); mr.u8(); mr.u32();
        }
        CHECK(mr.done());
    }

    b.clear();
    encodeError(b, 0, ErrMalformed, std::string(5000, 'x'));
    CHECK(b.size() == kHeaderSize + 4 + 2 + 2 + 1024);
    CHECK(errorCode(b) == ErrMalformed);
}

void testConditioning()
{
    float out = 99.0f;
    CHECK(!conditionAxis(std::numeric_limits<float>::quiet_NaN(), out) && out == 99.0f);
    CHECK(!conditionAxis(std::numeric_limits<float>::infinity(), out));
    CHECK(!conditionAxis(-std::numeric_limits<float>::infinity(), out));
    CHECK(conditionAxis(1.5f, out) && out == 1.0f);
    CHECK(conditionAxis(-7.0f, out) && out == -1.0f);
    CHECK(conditionAxis(1e30f, out) && out == 1.0f);
    CHECK(conditionAxis(0.009f, out) && out == 0.0f);
    CHECK(conditionAxis(-0.009f, out) && out == 0.0f);   // truncates toward zero
    CHECK(conditionAxis(0.5f, out) && out == 0.5f);
    CHECK(conditionAxis(0.129f, out) && out == 0.12f);
    CHECK(conditionAxis(-0.129f, out) && out == -0.12f);
    CHECK(conditionAxis(1.0f, out) && out == 1.0f);

    // The human chain quantizes twice (client store, server load). Every result is a whole number
    // of hundredths, and re-quantizing is a no-op.
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int differsFromSingleStep = 0;
    for (int i = 0; i < 200000; ++i)
    {
        const float v = u(rng);
        const float q = quantizeLikeHuman(v);
        CHECK(std::fabs(q) <= 1.0f);
        CHECK(std::fabs(q) <= std::fabs(v) + 1e-6f);
        CHECK(quantizeLikeHuman(q) == q);
        const float single = static_cast<float>(static_cast<short>(static_cast<int>(v * 100.0f))) / 100.0f;
        if (single != q)
            ++differsFromSingleStep;
    }
    std::printf("quantization: %d of 200000 random inputs differ between the contract's one-step formula and the two-step human chain\n",
                differsFromSingleStep);
}

void testConfig()
{
    std::vector<std::string> diags;
    Config c;
    CHECK(parseConfig("", c, diags) && c.listenHost == "127.0.0.1" && c.listenPort == 16700 && c.watchdogTicks == 6);
    CHECK(c.enterRange == 30.0f && c.spawnFallbackSeconds == 5 && c.spawnBlockedSeconds == 15 && !c.allowRoundRestart);

    Config cv1;
    diags.clear();
    CHECK(parseConfig("enterRange 42.5\nspawnFallbackSeconds = 3\nspawnBlockedSeconds 20\nallowRoundRestart 1\n", cv1, diags));
    CHECK(diags.empty() && cv1.enterRange == 42.5f && cv1.spawnFallbackSeconds == 3 && cv1.spawnBlockedSeconds == 20 &&
          cv1.allowRoundRestart);
    const char* badV1[] = { "enterRange 0", "enterRange 0.5", "enterRange 1001", "enterRange -5", "enterRange 1e3",
                            "enterRange 1.2.3", "enterRange .", "enterRange nan", "enterRange", "spawnFallbackSeconds 0",
                            "spawnFallbackSeconds 3601", "spawnFallbackSeconds 2.5", "spawnBlockedSeconds x",
                            "allowRoundRestart 2", "allowRoundRestart yes", "allowRoundRestart" };
    for (size_t i = 0; i < sizeof(badV1) / sizeof(badV1[0]); ++i)
    {
        Config cb;
        diags.clear();
        CHECK(!parseConfig(badV1[i], cb, diags));
        CHECK(cb.enterRange == 30.0f && cb.spawnFallbackSeconds == 5 && cb.spawnBlockedSeconds == 15 && !cb.allowRoundRestart);
    }

    Config c2;
    diags.clear();
    CHECK(parseConfig("# comment\nbotListen 127.0.0.1:16701\r\nwatchdogTicks = 9 // trailing\n", c2, diags));
    CHECK(c2.listenPort == 16701 && c2.watchdogTicks == 9 && diags.empty());

    Config c3;
    diags.clear();
    CHECK(parseConfig("botListen = 0.0.0.0:16700\nfoo bar\n", c3, diags));
    CHECK(c3.listenHost == "0.0.0.0" && diags.size() == 1);

    const char* bad[] = { "botListen 127.0.0.1:16567", "botListen localhost:16700", "botListen 127.0.0.1",
                          "botListen 127.0.0.1:0", "botListen 127.0.0.1:70000", "botListen 1.2.3:5",
                          "botListen 256.1.1.1:5", "watchdogTicks 0", "watchdogTicks -3", "watchdogTicks abc",
                          "watchdogTicks", "botListen" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        Config cb;
        diags.clear();
        CHECK(!parseConfig(bad[i], cb, diags));
        CHECK(cb.listenPort == 16700 && cb.watchdogTicks == 6);   // left at the default
    }
}

void testSession()
{
    // Hello must come first.
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> f = frame(MsgSetInput, setInputPayload(1, 0, 0, 0, 0));
        CHECK(!s.onBytes(f.data(), f.size(), reply));
        CHECK(errorCode(reply) == ErrMalformed);
        CHECK(drain(ch) == 0);
    }
    // Version mismatch.
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> f = frame(MsgHello, helloPayload(1, "x"));   // v1 is no longer accepted
        CHECK(!s.onBytes(f.data(), f.size(), reply));
        CHECK(errorCode(reply) == ErrVersion);
    }
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> f = frame(MsgHello, helloPayload(3, "x"));
        CHECK(!s.onBytes(f.data(), f.size(), reply));
        CHECK(errorCode(reply) == ErrVersion);
    }
    // RestartRound is a client message; module-only v1 types are unknown when a client sends them.
    {
        Channel ch;
        Session s(&ch, 4);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        std::vector<uint8_t> r = frame(MsgRestartRound, restartPayload(99));
        CHECK(s.onBytes(h.data(), h.size(), reply) && s.onBytes(r.data(), r.size(), reply) && reply.empty());
        std::deque<Command> q;
        ch.drainCommands(q);
        CHECK(q.size() == 2 && q[1].kind == Command::RestartRound && q[1].restartRound.requestId == 99 && q[1].conn == 4);
    }
    // v2 Teleport and Spot become commands; before Hello they close with code 2.
    {
        Channel ch;
        Session s(&ch, 5);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        std::vector<uint8_t> t = frame(MsgTeleport, teleportPayload(7, 254, 1, 2, 3, 45));
        std::vector<uint8_t> p = frame(MsgSpot, spotPayload(8, 254, 1819));
        CHECK(s.onBytes(h.data(), h.size(), reply) && s.onBytes(t.data(), t.size(), reply) &&
              s.onBytes(p.data(), p.size(), reply) && reply.empty());
        std::deque<Command> q;
        ch.drainCommands(q);
        CHECK(q.size() == 3 && q[1].kind == Command::Teleport && q[1].teleport.requestId == 7 &&
              q[1].teleport.pos[2] == 3.0f && q[1].teleport.yaw == 45.0f && q[2].kind == Command::Spot &&
              q[2].spot.targetId == 1819 && q[2].conn == 5);
    }
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> t = frame(MsgTeleport, teleportPayload(7, 254, 1, 2, 3, 45));
        CHECK(!s.onBytes(t.data(), t.size(), reply) && errorCode(reply) == ErrMalformed);
    }
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> r = frame(MsgRestartRound, restartPayload(1));
        CHECK(!s.onBytes(r.data(), r.size(), reply));   // before Hello
        CHECK(errorCode(reply) == ErrMalformed);
    }
    // Unknown and wrong-direction types; Hello twice.
    const uint16_t badTypes[] = { 0x9999, MsgWelcome, MsgTickState, MsgError, MsgAck, 0, MsgSpawnBlocked, MsgVehicleList,
                                  MsgBotRemoved, MsgProjectileList, MsgDamage, MsgKill, MsgChat, MsgRoundState };
    for (size_t i = 0; i < sizeof(badTypes) / sizeof(badTypes[0]); ++i)
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        CHECK(s.onBytes(h.data(), h.size(), reply) && reply.empty());
        std::vector<uint8_t> f = frame(badTypes[i], std::vector<uint8_t>(3, 1));
        CHECK(!s.onBytes(f.data(), f.size(), reply));
        CHECK(errorCode(reply) == ErrUnknownType);
    }
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        CHECK(s.onBytes(h.data(), h.size(), reply));
        CHECK(!s.onBytes(h.data(), h.size(), reply));
        CHECK(errorCode(reply) == ErrMalformed);
    }
    // A valid sequence, split at every byte boundary, yields the commands in order.
    {
        std::vector<uint8_t> stream = frame(MsgHello, helloPayload(kVersion, "x"));
        std::vector<uint8_t> cb;
        { Writer w(cb); w.u32(1); w.str("[BOT] t"); w.u8(1); }
        std::vector<uint8_t> f1 = frame(MsgCreateBot, cb);
        std::vector<uint8_t> f2 = frame(MsgSetInput, setInputPayload(255, 0.1f, 0, 0, 0));
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        for (size_t split = 0; split <= stream.size(); ++split)
        {
            Channel ch;
            Session s(&ch, 9);
            std::vector<uint8_t> reply;
            CHECK(s.onBytes(stream.data(), split, reply));
            CHECK(s.onBytes(stream.data() + split, stream.size() - split, reply));
            std::deque<Command> q;
            ch.drainCommands(q);
            CHECK(q.size() == 3 && q[0].kind == Command::Hello && q[1].kind == Command::CreateBot &&
                  q[2].kind == Command::SetInput && q[2].conn == 9 && q[1].createBot.name == "[BOT] t");
        }
    }
    // Malformed payloads of known types close with code 2.
    const uint16_t types[] = { MsgCreateBot, MsgEnterVehicle, MsgRemoveBot, MsgSetInput, MsgRestartRound, MsgTeleport, MsgSpot };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        s.onBytes(h.data(), h.size(), reply);
        std::vector<uint8_t> f = frame(types[i], std::vector<uint8_t>(1, 0));
        CHECK(!s.onBytes(f.data(), f.size(), reply));
        CHECK(errorCode(reply) == ErrMalformed);
    }
    // Oversize header.
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        uint8_t over[8] = { 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0, 0 };
        CHECK(!s.onBytes(over, sizeof(over), reply));
        CHECK(errorCode(reply) == ErrMalformed);
    }
    // A flood beyond the command queue bound closes the connection instead of growing memory.
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "x"));
        s.onBytes(h.data(), h.size(), reply);
        std::vector<uint8_t> f = frame(MsgSetInput, setInputPayload(1, 0, 0, 0, 0));
        bool closed = false;
        for (size_t i = 0; i < Channel::kMaxCommands + 10 && !closed; ++i)
            closed = !s.onBytes(f.data(), f.size(), reply);
        CHECK(closed && errorCode(reply) == ErrRefused);

        // The flood must not count against the next connection, whose Hello lands before the main
        // thread has drained the old commands.
        Command gone;
        gone.kind = Command::Disconnected;
        gone.conn = 1;
        CHECK(ch.pushCommand(gone));
        ch.closeConnection(1);
        ch.openConnection(2);
        Session s2(&ch, 2);
        std::vector<uint8_t> reply2;
        CHECK(s2.onBytes(h.data(), h.size(), reply2) && reply2.empty());
        std::deque<Command> q;
        ch.drainCommands(q);
        CHECK(q.size() == 2 && q[0].kind == Command::Disconnected && q[1].kind == Command::Hello && q[1].conn == 2);
    }
}

void testFuzz()
{
    std::mt19937 rng(12345);
    int closed = 0;
    for (int round = 0; round < 20000; ++round)
    {
        Channel ch;
        Session s(&ch, 1);
        std::vector<uint8_t> reply;
        bool open = true;
        if (round % 2)
        {
            std::vector<uint8_t> h = frame(MsgHello, helloPayload(kVersion, "fuzz"));
            open = s.onBytes(h.data(), h.size(), reply);
        }
        // Random frames with plausible headers, then raw noise.
        for (int k = 0; k < 8 && open; ++k)
        {
            std::vector<uint8_t> payload(rng() % 64);
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<uint8_t>(rng());
            const uint16_t types[] = { MsgHello, MsgCreateBot, MsgEnterVehicle, MsgRemoveBot, MsgSetInput,
                                       MsgRestartRound, MsgTeleport, MsgSpot, static_cast<uint16_t>(rng()) };
            std::vector<uint8_t> f = frame(types[rng() % 9], payload);
            if (rng() % 8 == 0)
                f[rng() % f.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
            open = s.onBytes(f.data(), f.size(), reply);
        }
        for (int k = 0; k < 4 && open; ++k)
        {
            std::vector<uint8_t> noise(rng() % 300);
            for (size_t i = 0; i < noise.size(); ++i)
                noise[i] = static_cast<uint8_t>(rng());
            open = s.onBytes(noise.data(), noise.size(), reply);
        }
        if (!open)
        {
            ++closed;
            CHECK(errorCode(reply) != 0);   // every close carries a well-formed Error frame
        }
        drain(ch);
    }
    std::printf("fuzz: 20000 sessions, %d closed with an Error frame, none crashed\n", closed);
}

void testChannel()
{
    Channel ch;
    std::vector<uint8_t> f = { 1, 2, 3 };
    ch.sendControl(1, f);                       // no connection yet: dropped
    std::vector<uint8_t> out;
    CHECK(!ch.takeOutbound(1, out));

    ch.openConnection(5);
    f = { 1, 2, 3 };
    ch.sendControl(4, f);                       // not open: dropped
    f = { 4 };
    ch.sendControl(5, f);
    std::vector<uint8_t> s1 = { 9, 9 };
    ch.sendState(5, s1);
    std::vector<uint8_t> s2 = { 8 };
    ch.sendState(5, s2);                        // replaces the unsent one
    CHECK(ch.takeOutbound(5, out) && out == std::vector<uint8_t>({ 4, 8 }));
    out.clear();
    CHECK(!ch.takeOutbound(5, out));

    // Slot order after the control frames: TickState, projectiles, VehicleList. Newer replaces unsent.
    std::vector<uint8_t> v1 = { 7 };
    ch.sendVehicles(5, v1);
    std::vector<uint8_t> v2 = { 6 };
    ch.sendVehicles(5, v2);
    std::vector<uint8_t> v3 = { 5 };
    ch.sendVehicles(4, v3);
    std::vector<uint8_t> p1 = { 10 };
    ch.sendLatest(5, Channel::SlotProjectiles, p1);
    std::vector<uint8_t> s3 = { 3 };
    ch.sendState(5, s3);
    f = { 2 };
    ch.sendControl(5, f);
    CHECK(ch.takeOutbound(5, out) && out == std::vector<uint8_t>({ 2, 3, 10, 6 }));
    out.clear();

    // Two connections are independent; closing one drops only its frames and commands.
    ch.openConnection(7);
    std::vector<uint8_t> a = { 1 };
    ch.sendState(5, a);
    std::vector<uint8_t> b = { 2 };
    ch.sendState(7, b);
    f = { 3 };
    ch.sendControl(7, f);
    CHECK(ch.openConnections() == 2);
    Command c5; c5.kind = Command::RemoveBot; c5.conn = 5;
    Command c7; c7.kind = Command::RemoveBot; c7.conn = 7;
    Command d7; d7.kind = Command::Disconnected; d7.conn = 7;
    CHECK(ch.pushCommand(c5) && ch.pushCommand(c7) && ch.pushCommand(d7));
    ch.closeConnection(7);
    CHECK(!ch.takeOutbound(7, out));
    CHECK(ch.takeOutbound(5, out) && out == std::vector<uint8_t>({ 1 }));
    out.clear();
    {
        std::deque<Command> q;
        ch.drainCommands(q);
        CHECK(q.size() == 2 && q[0].conn == 5 && q[1].kind == Command::Disconnected);
    }
    CHECK(ch.openConnections() == 1);

    std::vector<uint8_t> big(Channel::kMaxControlBytes + 1);
    ch.sendControl(5, big);
    CHECK(ch.controlOverflow(5));
    ch.openConnection(6);
    CHECK(!ch.controlOverflow(6));
    ch.closeConnection(5);
    CHECK(!ch.controlOverflow(5));

    for (size_t i = 0; i < Channel::kMaxLogLines + 5; ++i)
        ch.log("x");
    std::deque<std::string> logs;
    CHECK(ch.drainLogs(logs) == 5 && logs.size() == Channel::kMaxLogLines);
}


// The fields of IServerExtension::SpawnGroupInfo, without the engine header.
struct Group
{
    int id;
    int squad;
    int pointCount;
    int enterOnSpawnPoints;
    bool active;
    bool onlyForHuman;
    float x, y, z;
};

Group group(int id, int points, int enters, float x, bool active = true, int squad = 0, bool human = false)
{
    Group g = { id, squad, points, enters, active, human, x, 0.0f, 0.0f };
    return g;
}

void testSpawnPlan()
{
    using namespace serverext::spawn;

    // dalian_2_v_2-like: each base has a pad group and an in-heli enter-on-spawn point. A team
    // switch re-resolves from the other team's list, so nothing of the old team survives.
    const Group team1[] = { group(3, 4, 0, 0.0f), group(4, 1, 1, 14.0f) };
    const Group team2[] = { group(7, 1, 1, 514.0f), group(8, 4, 0, 500.0f) };
    Choice c = choose(team1, 2);
    CHECK(c.ok && c.home == 3 && c.target == 3);
    c = choose(team2, 2);
    CHECK(c.ok && c.home == 8 && c.target == 8);   // the on-foot group, not the lower-id heli point
    std::vector<int> order = fallbackOrder(c.home, c.target, team2, 2);
    CHECK(order.size() == 2 && order[0] == 8 && order[1] == 7);

    // Inactive groups still count: CreateBot and a team change succeed while the pad is blocked.
    const Group blocked[] = { group(8, 4, 0, 500.0f, false) };
    c = choose(blocked, 1);
    CHECK(c.ok && c.home == 8);

    // Only an enter-on-spawn group: no home, the target is that group.
    const Group heliOnly[] = { group(7, 1, 1, 514.0f) };
    c = choose(heliOnly, 1);
    CHECK(c.ok && c.home == -1 && c.target == 7);
    order = fallbackOrder(c.home, c.target, heliOnly, 1);
    CHECK(order.size() == 1 && order[0] == 7);

    // Nothing usable (squad, human-only, no points) or no groups at all: Error 6 / pending.
    const Group unusable[] = { group(1, 4, 0, 0.0f, true, 2), group(2, 4, 0, 0.0f, true, 0, true), group(5, 0, 0, 0.0f) };
    c = choose(unusable, 3);
    CHECK(!c.ok && c.home == -1 && c.target == -1);
    CHECK(fallbackOrder(c.home, c.target, unusable, 3).empty());
    c = choose(unusable, 0);
    CHECK(!c.ok);
    CHECK(choose(static_cast<const Group*>(0), 0).ok == false);

    // Order: home, then on-foot groups nearest home, then enter-on-spawn groups nearest home;
    // unusable groups never appear.
    const Group many[] = { group(1, 2, 1, 5.0f), group(2, 4, 0, 100.0f), group(3, 4, 0, 300.0f),
                           group(4, 4, 0, 50.0f, false), group(5, 4, 0, 10.0f, true, 1), group(6, 2, 2, 400.0f),
                           group(9, 4, 0, 200.0f) };
    c = choose(many, 7);
    CHECK(c.home == 2);
    order = fallbackOrder(c.home, c.target, many, 7);
    // From home at 100: on foot 4 (50 m, inactive but listed), 9 (100 m), 3 (200 m); then the
    // enter-on-spawn groups 1 (95 m), 6 (300 m). 5 is a squad group.
    CHECK(order.size() == 6);
    if (order.size() == 6)
    {
        CHECK(order[0] == 2);
        CHECK(order[1] == 4 && order[2] == 9 && order[3] == 3);
        CHECK(order[4] == 1 && order[5] == 6);
    }
    CHECK(findGroup(many, 7, 9) == &many[6] && findGroup(many, 7, 42) == 0);
}

// The level-and-hold neutral (v2 item 11) against the gains and signs of docs/findings/q5-q9.md.
void testNeutral()
{
    const float dt = 1.0f / 30.0f;
    // Level and still: cyclic and pedals 0, collective stays at the held value.
    {
        LevelAndHold n;
        n.start(0.39f);
        LevelAndHold::Output o = n.step(10.0f, 0.0f, 0.0f, 0.0f, dt);
        CHECK(o.pitch == 0.0f && o.roll == 0.0f && o.pedals == 0.0f && std::fabs(o.collective - 0.39f) < 1e-6f);
    }
    // Nose down 10 degrees: pull back (cyclic pitch negative). Bank left 20 degrees (roll +): roll right (+).
    {
        LevelAndHold n;
        n.start(0.0f);
        LevelAndHold::Output o = n.step(0.0f, 10.0f, 20.0f, 0.0f, dt);
        CHECK(std::fabs(o.pitch + 0.5f) < 1e-5f && std::fabs(o.roll - 1.0f) < 1e-5f);
    }
    // Rates: a rising nose is damped, a yaw rate to the right gives left pedal.
    {
        LevelAndHold n;
        n.start(0.0f);
        n.step(0.0f, 0.0f, 0.0f, 0.0f, dt);
        LevelAndHold::Output o = n.step(1.0f, -1.0f, 0.0f, 0.0f, dt);   // nose up 30 deg/s, yaw right 30 deg/s
        CHECK(o.pitch > 0.0f && o.pedals < 0.0f);
        // Yaw across the +-180 seam is a small rate, not a 360-degree jump.
        LevelAndHold m;
        m.start(0.0f);
        m.step(179.5f, 0.0f, 0.0f, 0.0f, dt);
        LevelAndHold::Output w = m.step(-179.5f, 0.0f, 0.0f, 0.0f, dt);
        CHECK(std::fabs(w.pedals + 0.9f) < 1e-3f);
    }
    // Climbing at 10 m/s from full collective: collective drops below the held value and the
    // integrator keeps lowering it while the climb lasts; a descent raises it.
    {
        LevelAndHold n;
        n.start(1.0f);
        LevelAndHold::Output a = n.step(0.0f, 0.0f, 0.0f, 10.0f, dt);
        LevelAndHold::Output b = n.step(0.0f, 0.0f, 0.0f, 10.0f, dt);
        CHECK(a.collective < 1.0f && b.collective < a.collective);
        LevelAndHold d;
        d.start(-0.08f);
        LevelAndHold::Output c = d.step(0.0f, 0.0f, 0.0f, -7.0f, dt);
        CHECK(c.collective > -0.08f);
    }
    // Outputs stay within -1..1; non-finite input keeps the held collective.
    {
        LevelAndHold n;
        n.start(5.0f);
        LevelAndHold::Output o = n.step(0.0f, 89.0f, -170.0f, -100.0f, dt);
        CHECK(o.pitch >= -1.0f && o.roll >= -1.0f && o.collective <= 1.0f && o.collective >= -1.0f);
        LevelAndHold::Output bad = n.step(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, dt);
        CHECK(bad.pitch == 0.0f && bad.roll == 0.0f && bad.pedals == 0.0f && bad.collective <= 1.0f);
    }
}

} // namespace

int main()
{
    testNeutral();
    testWriterReader();
    testDecoder();
    testParsers();
    testV2Encoders();
    testEncoders();
    testConditioning();
    testConfig();
    testSession();
    testFuzz();
    testChannel();
    testSpawnPlan();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
