#include "Session.h"

#include <cstdio>

namespace serverext {

using namespace proto;

bool Session::fail(std::vector<uint8_t>& reply, uint16_t code, const std::string& message)
{
    encodeError(reply, 0, code, message);
    m_closeReason = message;
    return false;
}

bool Session::push(Command& command, std::vector<uint8_t>& reply)
{
    command.conn = m_conn;
    if (!m_channel->pushCommand(command))
        return fail(reply, ErrRefused, "too many pending commands; the server is not keeping up");
    return true;
}

bool Session::onBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& reply)
{
    m_decoder.feed(data, size);
    while (true)
    {
        const FrameDecoder::Result r = m_decoder.next(m_frame);
        if (r == FrameDecoder::NeedMore)
            return true;
        if (r == FrameDecoder::Oversize)
            return fail(reply, ErrMalformed, "frame payload exceeds 65536 bytes");
        if (r == FrameDecoder::BadReserved)
            return fail(reply, ErrMalformed, "frame header reserved field is not 0");
        ++m_frames;
        if (!handleFrame(m_frame, reply))
            return false;
    }
}

bool Session::handleFrame(const Frame& frame, std::vector<uint8_t>& reply)
{
    if (!isClientMessage(frame.type))
    {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "unknown message type 0x%04X", static_cast<unsigned>(frame.type));
        return fail(reply, ErrUnknownType, msg);
    }

    Command c;
    if (!m_helloSeen)
    {
        if (frame.type != MsgHello)
            return fail(reply, ErrMalformed, "the first message must be Hello");
        if (!parseHello(frame.payload, c.hello))
            return fail(reply, ErrMalformed, "malformed Hello");
        if (c.hello.version != kVersion)
        {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "protocol version %u is not supported; the server speaks %u",
                          static_cast<unsigned>(c.hello.version), static_cast<unsigned>(kVersion));
            return fail(reply, ErrVersion, msg);
        }
        m_helloSeen = true;
        c.kind = Command::Hello;
        return push(c, reply);
    }

    switch (frame.type)
    {
    case MsgHello:
        return fail(reply, ErrMalformed, "Hello sent twice");
    case MsgCreateBot:
        if (!parseCreateBot(frame.payload, c.createBot))
            return fail(reply, ErrMalformed, "malformed CreateBot");
        c.kind = Command::CreateBot;
        break;
    case MsgEnterVehicle:
        if (!parseEnterVehicle(frame.payload, c.enterVehicle))
            return fail(reply, ErrMalformed, "malformed EnterVehicle");
        c.kind = Command::EnterVehicle;
        break;
    case MsgRemoveBot:
        if (!parseRemoveBot(frame.payload, c.removeBot))
            return fail(reply, ErrMalformed, "malformed RemoveBot");
        c.kind = Command::RemoveBot;
        break;
    case MsgSetInput:
        if (!parseSetInput(frame.payload, c.setInput))
            return fail(reply, ErrMalformed, "malformed SetInput");
        c.kind = Command::SetInput;
        break;
    default:
        return fail(reply, ErrUnknownType, "unexpected message type");
    }
    return push(c, reply);
}

} // namespace serverext
