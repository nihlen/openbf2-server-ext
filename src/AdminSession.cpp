#include "AdminSession.h"

#include <cstdio>
#include <cstring>

#include "AdminCrypto.h"

namespace serverext {
namespace admin {

AdminSession::AdminSession(AdminLink* link, uint32_t session, const std::string& serverId,
                           const std::vector<uint8_t>& key, const std::string& build)
    : m_link(link), m_session(session), m_serverId(serverId), m_key(key), m_build(build), m_nonceSet(false),
      m_started(false), m_authenticated(false), m_frames(0), m_peerError(0), m_decoder(kMaxInboundPayload)
{
    std::memset(m_nonce, 0, sizeof(m_nonce));
}

void AdminSession::setNonce(const uint8_t nonce[kNonceSize])
{
    std::memcpy(m_nonce, nonce, kNonceSize);
    m_nonceSet = true;
}

bool AdminSession::start(std::vector<uint8_t>& out)
{
    if (!m_nonceSet && !randomBytes(m_nonce, kNonceSize))
    {
        m_closeReason = "OpenSSL could not produce a random nonce";
        return false;
    }
    m_nonceSet = true;
    m_started = true;
    encodeHello(out, m_serverId, m_nonce, m_build);
    return true;
}

bool AdminSession::fail(std::vector<uint8_t>& out, uint16_t code, const std::string& message)
{
    if (code)
        encodeError(out, 0, code, message);
    m_closeReason = message;
    return false;
}

bool AdminSession::onBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    m_decoder.feed(data, size);
    while (true)
    {
        const proto::FrameDecoder::Result r = m_decoder.next(m_frame);
        if (r == proto::FrameDecoder::NeedMore)
            return true;
        if (r == proto::FrameDecoder::Oversize)
            return fail(out, ErrMalformed, "frame payload exceeds 65536 bytes");
        if (r == proto::FrameDecoder::BadReserved)
            return fail(out, ErrMalformed, "frame header reserved field is not 0");
        ++m_frames;
        if (!handleFrame(m_frame, out))
            return false;
    }
}

bool AdminSession::handleFrame(const proto::Frame& frame, std::vector<uint8_t>& out)
{
    const uint8_t* p = frame.payload.empty() ? 0 : &frame.payload[0];
    const size_t n = frame.payload.size();
    if (!isWebAdminMessage(frame.type))
    {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "unknown message type 0x%04X", static_cast<unsigned>(frame.type));
        return fail(out, ErrUnknownType, msg);
    }

    if (frame.type == MsgError)
    {
        Command e;
        if (!parseCommand(MsgError, p, n, e, 0))
            return fail(out, ErrMalformed, "malformed Error");
        m_peerError = e.index;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "WebAdmin sent Error %u: ", static_cast<unsigned>(e.index));
        // Errors that end the handshake or the protocol close the link; others are logged by the
        // main thread through the command queue.
        if (!m_authenticated || e.index == ErrVersion || e.index == ErrMalformed || e.index == ErrUnknownType ||
            e.index == ErrAuth)
            return fail(out, 0, std::string(msg) + e.text.substr(0, 200));
        e.session = m_session;
        if (!m_link->pushCommand(e))
            return fail(out, ErrRefused, "too many pending commands; the server is not keeping up");
        return true;
    }

    if (!m_authenticated)
    {
        if (!m_started || frame.type != MsgChallenge)
            return fail(out, ErrMalformed, "the first message must be Challenge");
        ChallengeMsg c;
        if (!parseChallenge(p, n, c))
            return fail(out, ErrMalformed, "malformed Challenge");
        uint8_t expected[kProofSize];
        if (!hmacSha256(m_key, proofTranscript(kWebAdminProofLabel, m_serverId, m_nonce, c.nonce), expected))
            return fail(out, ErrAuth, "HMAC failed");
        if (!equalConstantTime(expected, c.proof, kProofSize))
            return fail(out, ErrAuth, "WebAdmin's proof does not match adminKey");
        uint8_t proof[kProofSize];
        if (!hmacSha256(m_key, proofTranscript(kServerProofLabel, m_serverId, m_nonce, c.nonce), proof))
            return fail(out, ErrAuth, "HMAC failed");
        encodeAuth(out, proof);
        m_authenticated = true;
        m_link->openSession(m_session);
        Command up;
        up.type = Command::LinkUp;
        up.session = m_session;
        m_link->pushCommand(up);
        return true;
    }

    if (frame.type == MsgChallenge)
        return fail(out, ErrMalformed, "Challenge sent twice");
    if (frame.type == MsgPong)
    {
        Command pong;
        if (!parseCommand(MsgPong, p, n, pong, 0))
            return fail(out, ErrMalformed, "malformed Pong");
        return true;   // its arrival already counted as traffic
    }

    Command c;
    std::string invalid;
    if (!parseCommand(frame.type, p, n, c, &invalid))
    {
        if (invalid.empty())
        {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "malformed message 0x%04X", static_cast<unsigned>(frame.type));
            return fail(out, ErrMalformed, msg);
        }
        // A well-formed command with a bad value: the main thread answers Error 7 in order.
        c = Command();
        c.type = frame.type;
        proto::Reader r(p, n);
        c.requestId = r.u32();
        c.invalid = invalid;
    }
    c.session = m_session;
    if (!m_link->pushCommand(c))
        return fail(out, ErrRefused, "too many pending commands; the server is not keeping up");
    return true;
}

} // namespace admin
} // namespace serverext
