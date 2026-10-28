// The admin channel's only state shared between the main thread and the admin I/O thread: the
// inbound command queue and the outbound queue of the current link session. Every member function
// takes one short mutex hold and never blocks on I/O. Engine-free (POSIX pipe for the wake-up).
//
// Outbound order: queued frames (replies, snapshots and events) in the order they were queued, then
// the latest frame of each stream slot. Events are dropped instead of queued once the queue holds
// queueBytes; their sequence numbers are used up, so WebAdmin sees the gap and resynchronises.
// Replies and snapshots are always queued; at four times queueBytes the link is overloaded and the
// I/O thread closes it.

#ifndef SERVEREXT_ADMINLINK_H
#define SERVEREXT_ADMINLINK_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "AdminProtocol.h"

namespace serverext {
namespace admin {

class AdminLink
{
public:
    static const size_t kMaxCommands = 4096;
    enum Slot { SlotPlayers = 0, SlotVehicles = 1, SlotProjectiles = 2, SlotGame = 3, SlotAim = 4, SlotCount = 5 };

    explicit AdminLink(size_t queueBytes);
    ~AdminLink();

    // --- main thread ---
    // Moves every pending command into out (which must be empty).
    void drainCommands(std::deque<Command>& out);
    // Queues a reply, Welcome or Snapshot for session. Dropped only if session is not the open one.
    void sendReply(uint32_t session, std::vector<uint8_t>& frame);
    // Queues an event. @return False when it was dropped (queue full or session not open).
    bool sendEvent(uint32_t session, std::vector<uint8_t>& frame);
    // Replaces the pending frame of a stream slot; frame receives the previous buffer back.
    void sendLatest(uint32_t session, Slot slot, std::vector<uint8_t>& frame);

    // --- I/O thread ---
    // Starts accepting frames for session and discards anything left from the previous one.
    void openSession(uint32_t session);
    void closeSession();
    // @return False when the command queue is full; the caller closes the link.
    bool pushCommand(Command& command);
    // Appends the queued frames, then the latest stream frames, to out.
    // @return True if anything was appended.
    bool takeOutbound(std::vector<uint8_t>& out);
    // True once queued bytes reached four times queueBytes.
    bool overloaded() const;
    // Events dropped for a full queue since the last call.
    uint64_t takeDroppedEvents();
    int wakeReadFd() const { return m_wake[0]; }
    // Wakes the I/O thread (also used by the main thread's sends).
    void wake();

private:
    mutable std::mutex m_mutex;
    size_t m_queueBytes;
    uint32_t m_session;         // 0 while no link is open
    std::deque<Command> m_commands;
    std::deque<std::vector<uint8_t> > m_queue;
    size_t m_queued;
    bool m_overloaded;
    uint64_t m_dropped;
    std::vector<uint8_t> m_latest[SlotCount];
    bool m_hasLatest[SlotCount];
    int m_wake[2];
};

} // namespace admin
} // namespace serverext

#endif // SERVEREXT_ADMINLINK_H
