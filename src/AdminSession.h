// Per-link protocol state of the admin channel on the admin I/O thread: the Hello / Challenge /
// Auth handshake, framing, and parsing of WebAdmin's messages into commands for the main thread.
// Socket-free, TLS-free and engine-free, so hostile input can be unit-tested.

#ifndef SERVEREXT_ADMINSESSION_H
#define SERVEREXT_ADMINSESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "AdminLink.h"
#include "AdminProtocol.h"

namespace serverext {
namespace admin {

class AdminSession
{
public:
    // key is the raw adminKey. session is this link's number (Welcome's session field).
    AdminSession(AdminLink* link, uint32_t session, const std::string& serverId, const std::vector<uint8_t>& key,
                 const std::string& build);

    // For tests: use this module nonce instead of a random one. Call before start().
    void setNonce(const uint8_t nonce[kNonceSize]);

    // Appends the Hello frame to out. @return False when no random nonce could be made.
    bool start(std::vector<uint8_t>& out);

    // Consumes received plaintext. Frames the module must send at once (Auth, a final Error) are
    // appended to out. On authentication the link session is opened and a LinkUp command queued.
    // @return False when the link must be closed; closeReason() says why.
    bool onBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

    bool authenticated() const { return m_authenticated; }
    const std::string& closeReason() const { return m_closeReason; }
    uint64_t frames() const { return m_frames; }
    // WebAdmin's last Error code and message, if it sent one.
    uint16_t peerErrorCode() const { return m_peerError; }

private:
    bool fail(std::vector<uint8_t>& out, uint16_t code, const std::string& message);
    bool handleFrame(const proto::Frame& frame, std::vector<uint8_t>& out);

    AdminLink* m_link;
    uint32_t m_session;
    std::string m_serverId;
    std::vector<uint8_t> m_key;
    std::string m_build;
    uint8_t m_nonce[kNonceSize];
    bool m_nonceSet;
    bool m_started;
    bool m_authenticated;
    uint64_t m_frames;
    uint16_t m_peerError;
    proto::FrameDecoder m_decoder;
    proto::Frame m_frame;
    std::string m_closeReason;
};

} // namespace admin
} // namespace serverext

#endif // SERVEREXT_ADMINSESSION_H
