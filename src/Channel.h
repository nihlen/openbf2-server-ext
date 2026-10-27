// The only state shared between the main thread and the I/O thread: the inbound command queue, the
// outbound frame queues of every open connection and the log queue. Every member function takes one
// short mutex hold and never blocks on I/O. Engine-free.

#ifndef SERVEREXT_CHANNEL_H
#define SERVEREXT_CHANNEL_H

#include <cstdint>
#include <chrono>
#include <deque>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "Protocol.h"

namespace serverext {

// One inbound item. conn identifies the TCP connection; it increases with every accepted one.
struct Command
{
    enum Kind { Connected, Disconnected, Hello, CreateBot, EnterVehicle, RemoveBot, SetInput, RestartRound, Teleport,
                Spot, SetTargetDirection, Say };

    Kind kind;
    uint32_t conn;
    proto::HelloMsg hello;
    proto::CreateBotMsg createBot;
    proto::EnterVehicleMsg enterVehicle;
    proto::RemoveBotMsg removeBot;
    proto::SetInputMsg setInput;
    proto::RestartRoundMsg restartRound;
    proto::TeleportMsg teleport;
    proto::SpotMsg spot;
    proto::SayMsg say;
    proto::SetTargetDirectionMsg targetDirection;
};

class Channel
{
public:
    // Bounds that protect the server from a flooding or non-reading sidecar.
    static const size_t kMaxCommands = 4096;
    static const size_t kMaxControlBytes = 8 * 1024 * 1024;
    static const size_t kMaxLogLines = 10000;

    // The per-tick frames that replace an unsent predecessor, in the order they are written after
    // the control frames.
    enum Slot { SlotState = 0, SlotProjectiles = 1, SlotVehicles = 2, SlotCount = 3 };

    Channel();

    // --- main thread ---
    // Moves every pending command into out (which must be empty) without copying.
    void drainCommands(std::deque<Command>& out);
    // Queues a control frame (Welcome, replies, events, Error). Never dropped while the connection
    // is open; dropped if conn is not open.
    void sendControl(uint32_t conn, std::vector<uint8_t>& frame);
    // Replaces the pending frame of slot for conn; an unsent older one is dropped. frame receives
    // the previous buffer back so the caller can reuse its capacity.
    void sendLatest(uint32_t conn, Slot slot, std::vector<uint8_t>& frame);
    void sendState(uint32_t conn, std::vector<uint8_t>& frame) { sendLatest(conn, SlotState, frame); }
    void sendVehicles(uint32_t conn, std::vector<uint8_t>& frame) { sendLatest(conn, SlotVehicles, frame); }

    // --- any thread ---
    void log(const std::string& line);

    // --- I/O thread ---
    // Returns false when the queue is full; the caller then closes the connection.
    bool pushCommand(Command& command);
    // Opens an outbound stream for conn.
    void openConnection(uint32_t conn);
    // Drops conn's outbound stream and its queued commands (connection lifecycle events excepted).
    void closeConnection(uint32_t conn);
    // Appends conn's queued control frames, then its latest per-tick frames in slot order, to out.
    // @return True if anything was appended.
    bool takeOutbound(uint32_t conn, std::vector<uint8_t>& out);
    bool controlOverflow(uint32_t conn) const;
    size_t openConnections() const;
    // Moves pending log lines into out; returns the number dropped since the last call.
    size_t drainLogs(std::deque<std::string>& out);

    // --- stepped time (docs/protocol.md "v4: stepped time", R3) ---
    static const uint32_t kMaxStepBudget = 300;
    // How long takeTicks waits for a grant or the next real-time tick (us), in place of the engine's 1 ms frame sleep
    static const int kStepWaitMicros = 2000;
    // Main thread, once at load (serverext.cfg stepMode 1): from now on takeTicks decides the ticks.
    void enableStepping();
    bool steppingEnabled() const;
    // I/O thread: conn grants ticks (0 releases stepping). The first grant makes conn the stepping
    // connection. @return False with why when stepping is off or another connection steps.
    bool step(uint32_t conn, uint32_t ticks, std::string& why);
    // Main thread, once per frame (the engine's tick source): the ticks this frame runs, 0..maxTicks.
    // From the stepping connection's budget; without one, 30 Hz of a steady clock (a backlog beyond
    // maxTicks is dropped, as TickCalculator does). With none due it waits up to kStepWaitMicros: for a
    // grant (step wakes it) or for the next real-time tick (docs/protocol.md v4, "Waiting for a grant").
    int takeTicks(int maxTicks);
    // The stepping connection, 0 if none.
    uint32_t steppingConnection() const;

    // The read end the I/O thread polls; one byte is written to the write end per send.
    int wakeReadFd() const { return m_wake[0]; }

private:
    struct Outbound
    {
        std::deque<std::vector<uint8_t> > control;
        size_t controlBytes;
        bool overflow;
        std::vector<uint8_t> latest[SlotCount];
        bool hasLatest[SlotCount];

        Outbound() : controlBytes(0), overflow(false)
        {
            for (int i = 0; i < SlotCount; ++i)
                hasLatest[i] = false;
        }
    };

    void wake();
    void releaseStepping();

    mutable std::mutex m_mutex;
    std::deque<Command> m_commands;
    std::map<uint32_t, Outbound> m_out;
    std::deque<std::string> m_logs;
    size_t m_logsDropped;
    int m_wake[2];

    // Stepped time, under m_mutex
    std::condition_variable m_stepWake;   // step() and closeConnection() wake a waiting takeTicks
    bool m_stepEnabled;
    uint32_t m_stepOwner;
    uint32_t m_stepBudget;
    std::chrono::steady_clock::time_point m_realTimeOrigin;
    uint64_t m_realTimeTicks;   // ticks granted since m_realTimeOrigin
};

} // namespace serverext

#endif // SERVEREXT_CHANNEL_H
