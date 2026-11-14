// Seat preferences for enter-on-spawn points (docs/admin-channel.md "Seat preferences"). Engine-free so
// the unit tests can drive it: the admin channel fills one Entry per player each tick, plan() says whose
// spawnAtTick to move, and the module writes them back with IPlayer::setSpawnAtTick.
//
// Why it works: the engine's spawn pass (ServerGameLogic::uPlayingSpawning) runs every third tick and spawns
// every due dead player in player-list order, and a spawn into a vehicle takes the first free seat in seat
// order. Two teammates due in the same pass therefore get seat 0 and seat 1 in join order. plan() makes the
// one who wants the later seat due at least three ticks after the one who wants the earlier seat, so the two
// can never share a pass, whatever the pass phase.

#ifndef SERVEREXT_SEATORDER_H
#define SERVEREXT_SEATORDER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace serverext {
namespace seat {

// Wire values (SeatPreference's seat).
const uint8_t kFirst = 0;     // the first seat (a helicopter's pilot)
const uint8_t kSecond = 1;    // the second seat (its gunner)
const uint8_t kNone = 255;    // no preference

inline bool validPreference(unsigned v) { return v == kFirst || v == kSecond || v == kNone; }

// Lower spawns first: a first-seat preference, then no preference, then a second-seat preference.
inline int rank(uint8_t preference)
{
    return preference == kFirst ? 0 : preference == kSecond ? 2 : 1;
}

// Tick numbers are the engine's 32-bit counter; compare by signed difference.
inline bool after(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
inline uint32_t later(uint32_t a, uint32_t b) { return after(a, b) ? a : b; }

struct Entry
{
    int id;                // player id
    int team;
    int group;             // IPlayer::getSpawnGroup; only > 0 takes part
    bool dead;             // !IPlayer::getIsAlive (a man-down player is alive and does not take part)
    uint32_t spawnAtTick;  // IPlayer::getSpawnAtTick; plan() updates it
    uint8_t preference;    // kFirst, kSecond or kNone
};

struct Delay
{
    int player;            // the player whose spawn moved
    int after;             // the teammate it now spawns after (the one that set the new tick)
    uint32_t from, to;     // its spawnAtTick before and after
    uint32_t afterTick;    // that teammate's spawnAtTick
};

// The minimum gap, in ticks, between two teammates' due ticks: one spawn pass (every third tick).
const uint32_t kGap = 3;

// now: the engine tick count (WorldPref::getTickCount) at the module's tick hook. The hook runs after this
// tick's spawn pass, so the next pass sees now + 1 or later; a dead player whose spawnAtTick is past is spawned
// by the first pass from now + 1 on. For every pair X, Y of dead players on the same team with the same spawn
// group > 0 and rank(X) > rank(Y): base = max(Y.spawnAtTick, now + 1); when X.spawnAtTick <= base + 2,
// X.spawnAtTick becomes base + 3. Y is spawned by the first pass at or after base, which is at most base + 2, and
// X by a pass at or after base + 3, so strictly later. Equal ranks are left to the engine's order. Ranks are a
// strict order, so the players are settled in rank order and one call is a fixed point: a second call with the
// same now changes nothing.
//
// players is updated in place; out receives one Delay per moved player (cleared first). Both vectors are the
// caller's, so a caller that keeps them allocates nothing per tick.
inline void plan(std::vector<Entry>& players, uint32_t now, std::vector<Delay>& out)
{
    out.clear();
    const uint32_t next = now + 1;
    for (int r = 1; r <= 2; ++r)
        for (size_t i = 0; i < players.size(); ++i)
        {
            Entry& x = players[i];
            if (!x.dead || x.group <= 0 || rank(x.preference) != r)
                continue;
            uint32_t need = x.spawnAtTick;
            int by = -1;
            uint32_t byTick = 0;
            for (size_t j = 0; j < players.size(); ++j)
            {
                const Entry& y = players[j];
                if (j == i || !y.dead || y.group != x.group || y.team != x.team || rank(y.preference) >= r)
                    continue;
                const uint32_t base = later(y.spawnAtTick, next);
                if (!after(base + kGap, need))
                    continue;
                need = base + kGap;
                by = y.id;
                byTick = y.spawnAtTick;
            }
            if (by < 0)
                continue;
            Delay d;
            d.player = x.id;
            d.after = by;
            d.from = x.spawnAtTick;
            d.to = need;
            d.afterTick = byTick;
            out.push_back(d);
            x.spawnAtTick = need;
        }
}

// True when plan() could move anyone: two dead players on one team and spawn group with different ranks.
inline bool anyPair(const std::vector<Entry>& players)
{
    for (size_t i = 0; i < players.size(); ++i)
    {
        const Entry& x = players[i];
        if (!x.dead || x.group <= 0)
            continue;
        for (size_t j = i + 1; j < players.size(); ++j)
        {
            const Entry& y = players[j];
            if (y.dead && y.group == x.group && y.team == x.team && rank(y.preference) != rank(x.preference))
                return true;
        }
    }
    return false;
}

} // namespace seat
} // namespace serverext

#endif
