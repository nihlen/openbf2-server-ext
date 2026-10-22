// Per-connection protocol state on the I/O thread: framing, the Hello-first rule and payload
// parsing. Socket-free and engine-free, so hostile input can be unit-tested.

#ifndef SERVEREXT_SESSION_H
#define SERVEREXT_SESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Channel.h"
#include "Protocol.h"

namespace serverext {

class Session
{
public:
    Session(Channel* channel, uint32_t conn) : m_channel(channel), m_conn(conn), m_helloSeen(false), m_frames(0) {}

    // Consumes received bytes and queues every complete, valid command.
    // @param reply Receives an Error frame to send before closing, if any.
    // @return False when the connection must be closed; closeReason() says why.
    bool onBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& reply);

    const std::string& closeReason() const { return m_closeReason; }
    uint64_t frames() const { return m_frames; }

private:
    bool fail(std::vector<uint8_t>& reply, uint16_t code, const std::string& message);
    bool handleFrame(const proto::Frame& frame, std::vector<uint8_t>& reply);
    bool push(Command& command, std::vector<uint8_t>& reply);

    Channel* m_channel;
    uint32_t m_conn;
    bool m_helloSeen;
    uint64_t m_frames;
    proto::FrameDecoder m_decoder;
    proto::Frame m_frame;
    std::string m_closeReason;
};

} // namespace serverext

#endif // SERVEREXT_SESSION_H
