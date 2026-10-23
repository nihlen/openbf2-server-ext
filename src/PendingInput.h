// The SetInput values waiting for a bot's next tick (docs/protocol.md v3 item 1). Engine-free, so
// the merge rule is unit-tested.

#ifndef SERVEREXT_PENDINGINPUT_H
#define SERVEREXT_PENDINGINPUT_H

#include <cstdint>

#include "Protocol.h"

namespace serverext {

// Collects the SetInputs that arrive for one bot before a tick applies them. The axes are latest
// wins; the buttons of every SetInput since the last apply are ORed, so a one-tick press survives a
// later SetInput in the same tick.
struct PendingInput
{
    float axes[proto::AxisCount];
    uint32_t buttons;
    bool has;

    PendingInput() : buttons(0), has(false)
    {
        for (int i = 0; i < proto::AxisCount; ++i)
            axes[i] = 0.0f;
    }

    // Adds one conditioned SetInput. Returns true when it superseded an unapplied one; kept receives
    // the buttons the merge saved (pressed before, not in this SetInput), 0 otherwise.
    bool add(const float (&newAxes)[proto::AxisCount], uint32_t newButtons, uint32_t& kept)
    {
        const bool superseded = has;
        kept = superseded ? (buttons & ~newButtons) : 0;
        for (int i = 0; i < proto::AxisCount; ++i)
            axes[i] = newAxes[i];
        buttons = superseded ? (buttons | newButtons) : newButtons;
        has = true;
        return superseded;
    }

    // Forgets anything unapplied (a disconnect or a death).
    void clear()
    {
        has = false;
        buttons = 0;
    }
};

} // namespace serverext

#endif // SERVEREXT_PENDINGINPUT_H
