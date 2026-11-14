// The watchdog neutral for a bot in a helicopter, docs/protocol.md "Watchdog" ("level and hold"):
// cyclic levels pitch and roll (P-D on the attitude), pedals damp the yaw rate, and collective is a
// PI on vertical speed starting from the last collective, so it needs no per-vehicle hover value.
// The gains and signs were tuned in flight on the stock attack helicopters.
// Engine-free, so it is unit-tested.

#ifndef SERVEREXT_NEUTRAL_H
#define SERVEREXT_NEUTRAL_H

#include <cmath>

namespace serverext {

class LevelAndHold
{
public:
    struct Output { float pitch, roll, collective, pedals; };

    LevelAndHold() : m_started(false), m_ci(0.0f), m_pitch(0.0f), m_roll(0.0f), m_yaw(0.0f) {}

    // Starts the law from the collective in use when the watchdog tripped.
    void start(float collective)
    {
        m_started = false;
        m_ci = clamp(collective);
    }

    // One tick. Angles in degrees as in TickState (pitch + nose down, roll + bank left, yaw 0 north),
    // vy in m/s (+ up), dt in seconds. Non-finite inputs give the held collective and zero cyclic.
    Output step(float yaw, float pitch, float roll, float vy, float dt)
    {
        Output o = { 0.0f, 0.0f, m_ci, 0.0f };
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll) || !std::isfinite(vy) ||
            !(dt > 0.0f))
            return o;
        float dp = 0.0f, dr = 0.0f, dyaw = 0.0f;
        if (m_started)
        {
            dp = (pitch - m_pitch) / dt;
            dr = (roll - m_roll) / dt;
            dyaw = wrap180(yaw - m_yaw) / dt;
        }
        m_started = true;
        m_pitch = pitch;
        m_roll = roll;
        m_yaw = yaw;
        // Cyclic pitch + is nose down (pitch +); cyclic roll + is bank right (roll -); pedals + yaw right.
        o.pitch = clamp(-kAtt * pitch - kAtt * dp);
        o.roll = clamp(kAtt * roll + kAtt * dr);
        o.pedals = clamp(-kYaw * dyaw);
        m_ci = clamp(m_ci - kVs * vy * dt);
        o.collective = clamp(m_ci - kVs * vy);
        return o;
    }

    static float clamp(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

    static float wrap180(float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    static constexpr float kAtt = 0.05f;   // cyclic per degree and per degree/s
    static constexpr float kYaw = 0.03f;   // pedals per degree/s of yaw rate
    static constexpr float kVs = 0.03f;    // collective per m/s, and per m of vertical travel (integral)

private:
    bool m_started;
    float m_ci;
    float m_pitch, m_roll, m_yaw;
};

} // namespace serverext

#endif // SERVEREXT_NEUTRAL_H
