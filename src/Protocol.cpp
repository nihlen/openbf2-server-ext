#include "Protocol.h"

#include <cmath>
#include <cstring>

namespace serverext {
namespace proto {

void Writer::begin(uint16_t type)
{
    m_start = m_out.size();
    u32(0);
    u16(type);
    u16(0);
}

void Writer::finish()
{
    const uint32_t len = static_cast<uint32_t>(m_out.size() - m_start - kHeaderSize);
    m_out[m_start + 0] = static_cast<uint8_t>(len);
    m_out[m_start + 1] = static_cast<uint8_t>(len >> 8);
    m_out[m_start + 2] = static_cast<uint8_t>(len >> 16);
    m_out[m_start + 3] = static_cast<uint8_t>(len >> 24);
}

void Writer::u16(uint16_t v)
{
    m_out.push_back(static_cast<uint8_t>(v));
    m_out.push_back(static_cast<uint8_t>(v >> 8));
}

void Writer::u32(uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        m_out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Writer::f32(float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
}

void Writer::f64(double v)
{
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 8; ++i)
        m_out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

void Writer::str(const std::string& s)
{
    const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
    u16(static_cast<uint16_t>(n));
    m_out.insert(m_out.end(), s.begin(), s.begin() + n);
}

bool Reader::need(size_t n)
{
    if (!m_ok || m_size - m_pos < n)
    {
        m_ok = false;
        return false;
    }
    return true;
}

uint8_t Reader::u8()
{
    if (!need(1))
        return 0;
    return m_data[m_pos++];
}

uint16_t Reader::u16()
{
    if (!need(2))
        return 0;
    const uint16_t v = static_cast<uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
    m_pos += 2;
    return v;
}

uint32_t Reader::u32()
{
    if (!need(4))
        return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(m_data[m_pos + i]) << (8 * i);
    m_pos += 4;
    return v;
}

float Reader::f32()
{
    const uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void Reader::str(std::string& out)
{
    const uint16_t n = u16();
    if (!need(n))
    {
        out.clear();
        return;
    }
    out.assign(reinterpret_cast<const char*>(m_data + m_pos), n);
    m_pos += n;
}

void FrameDecoder::feed(const uint8_t* data, size_t size)
{
    // Compact before growing so the buffer stays bounded.
    if (m_pos > 0 && m_pos == m_buf.size())
    {
        m_buf.clear();
        m_pos = 0;
    }
    else if (m_pos > kMaxPayload)
    {
        m_buf.erase(m_buf.begin(), m_buf.begin() + m_pos);
        m_pos = 0;
    }
    m_buf.insert(m_buf.end(), data, data + size);
}

FrameDecoder::Result FrameDecoder::next(Frame& out)
{
    const size_t avail = m_buf.size() - m_pos;
    if (avail < kHeaderSize)
        return NeedMore;
    const uint8_t* h = &m_buf[m_pos];
    const uint32_t len = static_cast<uint32_t>(h[0]) | (static_cast<uint32_t>(h[1]) << 8) |
                         (static_cast<uint32_t>(h[2]) << 16) | (static_cast<uint32_t>(h[3]) << 24);
    if (len > kMaxPayload)
        return Oversize;
    const uint16_t reserved = static_cast<uint16_t>(h[6] | (h[7] << 8));
    if (reserved != 0)
        return BadReserved;
    if (avail < kHeaderSize + len)
        return NeedMore;
    out.type = static_cast<uint16_t>(h[4] | (h[5] << 8));
    out.payload.assign(h + kHeaderSize, h + kHeaderSize + len);
    m_pos += kHeaderSize + len;
    return GotFrame;
}

bool parseHello(const std::vector<uint8_t>& p, HelloMsg& out)
{
    Reader r(p.data(), p.size());
    out.version = r.u16();
    r.str(out.name);
    return r.done();
}

bool parseCreateBot(const std::vector<uint8_t>& p, CreateBotMsg& out)
{
    Reader r(p.data(), p.size());
    out.requestId = r.u32();
    r.str(out.name);
    out.team = r.u8();
    return r.done();
}

bool parseEnterVehicle(const std::vector<uint8_t>& p, EnterVehicleMsg& out)
{
    Reader r(p.data(), p.size());
    out.requestId = r.u32();
    out.playerId = r.u8();
    out.vehicleId = r.u32();
    out.seat = r.u8();
    return r.done();
}

bool parseRemoveBot(const std::vector<uint8_t>& p, RemoveBotMsg& out)
{
    Reader r(p.data(), p.size());
    out.requestId = r.u32();
    out.playerId = r.u8();
    return r.done();
}

bool parseSetInput(const std::vector<uint8_t>& p, SetInputMsg& out)
{
    Reader r(p.data(), p.size());
    out.playerId = r.u8();
    out.tick = r.u32();
    for (int i = 0; i < AxisCount; ++i)
        out.axes[i] = r.f32();
    out.buttons = r.u32();
    return r.done();
}

bool isClientMessage(uint16_t type)
{
    switch (type)
    {
    case MsgHello:
    case MsgCreateBot:
    case MsgEnterVehicle:
    case MsgRemoveBot:
    case MsgSetInput:
        return true;
    default:
        return false;
    }
}

void encodeWelcome(std::vector<uint8_t>& out, float tickRate, uint32_t tick, const std::string& map,
                   const std::string& mode)
{
    Writer w(out);
    w.begin(MsgWelcome);
    w.u16(kVersion);
    w.f32(tickRate);
    w.u32(tick);
    w.str(map);
    w.str(mode);
    w.finish();
}

void encodeBotCreated(std::vector<uint8_t>& out, uint32_t requestId, uint8_t playerId)
{
    Writer w(out);
    w.begin(MsgBotCreated);
    w.u32(requestId);
    w.u8(playerId);
    w.finish();
}

void encodeAck(std::vector<uint8_t>& out, uint32_t requestId)
{
    Writer w(out);
    w.begin(MsgAck);
    w.u32(requestId);
    w.finish();
}

void encodeBotRemoved(std::vector<uint8_t>& out, uint8_t playerId, uint8_t reason)
{
    Writer w(out);
    w.begin(MsgBotRemoved);
    w.u8(playerId);
    w.u8(reason);
    w.finish();
}

void encodeError(std::vector<uint8_t>& out, uint32_t requestId, uint16_t code, const std::string& message)
{
    Writer w(out);
    w.begin(MsgError);
    w.u32(requestId);
    w.u16(code);
    w.str(message.size() > 1024 ? message.substr(0, 1024) : message);
    w.finish();
}

void encodeTickState(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<StateEntry>& entries)
{
    const size_t n = entries.size() > 255 ? 255 : entries.size();
    Writer w(out);
    w.begin(MsgTickState);
    w.u32(tick);
    w.f64(time);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i)
    {
        const StateEntry& e = entries[i];
        w.u8(e.playerId);
        w.u8(e.flags);
        w.u8(e.team);
        w.u8(e.seat);
        w.u32(e.rootId);
        for (int k = 0; k < 3; ++k) w.f32(e.pos[k]);
        for (int k = 0; k < 3; ++k) w.f32(e.ypr[k]);
        for (int k = 0; k < 3; ++k) w.f32(e.vel[k]);
        for (int k = 0; k < 3; ++k) w.f32(e.angVel[k]);
        w.f32(e.health);
    }
    w.finish();
}

float convertAxisToInternalResolution(float value)
{
    float scaled = value * 100.0f;
    if (scaled > 32767.0f)
        scaled = 32767.0f;
    else if (!(scaled >= -32767.0f))
        scaled = -32767.0f;
    return static_cast<float>(static_cast<short>(static_cast<int>(scaled))) / 100.0f;
}

float quantizeLikeHuman(float value)
{
    const short stored = static_cast<short>(static_cast<int>(convertAxisToInternalResolution(value) * 100.0f));
    return static_cast<float>(stored) / 100.0f;
}

bool conditionAxis(float value, float& out)
{
    if (!std::isfinite(value))
        return false;
    if (value > 1.0f)
        value = 1.0f;
    else if (value < -1.0f)
        value = -1.0f;
    out = quantizeLikeHuman(value);
    return true;
}

} // namespace proto
} // namespace serverext
