// The only state shared between the main thread and the I/O thread: the inbound command queue, the
// outbound frame queues and the log queue. Every member function takes one short mutex hold and
// never blocks on I/O. Engine-free.

#ifndef SERVEREXT_CHANNEL_H
#define SERVEREXT_CHANNEL_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Protocol.h"

namespace serverext {

// One inbound item. conn identifies the TCP connection; it increases with every accepted one.
struct Command
{
    enum Kind { Connected, Disconnected, Hello, CreateBot, EnterVehicle, RemoveBot, SetInput };

    Kind kind;
    uint32_t conn;
    proto::HelloMsg hello;
    proto::CreateBotMsg createBot;
    proto::EnterVehicleMsg enterVehicle;
    proto::RemoveBotMsg removeBot;
    proto::SetInputMsg setInput;
};

class Channel
{
public:
    // Bounds that protect the server from a flooding or non-reading sidecar.
    static const size_t kMaxCommands = 4096;
    static const size_t kMaxControlBytes = 8 * 1024 * 1024;
    static const size_t kMaxLogLines = 10000;

    Channel();

    // --- main thread ---
    // Moves every pending command into out (which must be empty) without copying.
    void drainCommands(std::deque<Command>& out);
    // Queues a control frame (Welcome, BotCreated, Ack, BotRemoved, Error). Never dropped while
    // the connection lives; dropped if conn is no longer the current connection.
    void sendControl(uint32_t conn, std::vector<uint8_t>& frame);
    // Replaces the pending TickState with frame; an unsent older one is dropped. frame receives the
    // previous buffer back so the caller can reuse its capacity.
    void sendState(uint32_t conn, std::vector<uint8_t>& frame);

    // --- any thread ---
    void log(const std::string& line);

    // --- I/O thread ---
    // Returns false when the queue is full; the caller then closes the connection.
    bool pushCommand(Command& command);
    // Starts a new outbound stream for conn and drops anything queued for an older one, in both
    // directions (connection lifecycle events excepted).
    void setConnection(uint32_t conn);
    // Appends the queued control frames, then the latest TickState, to out.
    // @return True if anything was appended.
    bool takeOutbound(std::vector<uint8_t>& out);
    bool controlOverflow() const;
    // Moves pending log lines into out; returns the number dropped since the last call.
    size_t drainLogs(std::deque<std::string>& out);

    // The read end the I/O thread polls; one byte is written to the write end per send.
    int wakeReadFd() const { return m_wake[0]; }

private:
    void wake();

    mutable std::mutex m_mutex;
    std::deque<Command> m_commands;
    uint32_t m_conn;
    std::deque<std::vector<uint8_t> > m_control;
    size_t m_controlBytes;
    bool m_overflow;
    std::vector<uint8_t> m_state;
    bool m_hasState;
    std::deque<std::string> m_logs;
    size_t m_logsDropped;
    int m_wake[2];
};

} // namespace serverext

#endif // SERVEREXT_CHANNEL_H
