// Wire format of the bot channel, docs/protocol.md v0. Engine-free: the I/O thread, the main thread
// and the unit tests all use it.

#ifndef SERVEREXT_PROTOCOL_H
#define SERVEREXT_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace serverext {
namespace proto {

const uint16_t kVersion = 0;
const size_t kHeaderSize = 8;
const uint32_t kMaxPayload = 64 * 1024;

enum MsgType : uint16_t
{
    MsgHello        = 0x0001,
    MsgWelcome      = 0x0002,
    MsgCreateBot    = 0x0101,
    MsgBotCreated   = 0x0102,
    MsgEnterVehicle = 0x0103,
    MsgRemoveBot    = 0x0104,
    MsgAck          = 0x0105,
    MsgBotRemoved   = 0x0106,
    MsgSetInput     = 0x0201,
    MsgTickState    = 0x0301,
    MsgError        = 0xFFFF
};

enum ErrorCode : uint16_t
{
    ErrVersion       = 1,
    ErrMalformed     = 2,
    ErrUnknownType   = 3,
    ErrUnknownPlayer = 4,
    ErrUnavailable   = 5,
    ErrRefused       = 6,
    ErrInvalidValue  = 7
};

enum RemoveReason : uint8_t
{
    ReasonRemoveBot = 1,
    ReasonMapChange = 2,
    ReasonEngine    = 3
};

// TickState entry flags.
const uint8_t kFlagAlive     = 1;
const uint8_t kFlagOwnBot    = 2;
const uint8_t kFlagInVehicle = 4;
const uint8_t kSeatOnFoot    = 255;

// Appends little-endian values to a byte buffer. begin() writes a frame header whose length
// finish() patches, so several frames can be appended to one buffer.
class Writer
{
public:
    explicit Writer(std::vector<uint8_t>& out) : m_out(out), m_start(0) {}

    void begin(uint16_t type);
    void finish();

    void u8(uint8_t v) { m_out.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void f32(float v);
    void f64(double v);
    // u16 byte length then the bytes; longer strings are truncated to 65535 bytes.
    void str(const std::string& s);

private:
    std::vector<uint8_t>& m_out;
    size_t m_start;
};

// Reads little-endian values from a payload. Any read past the end sets a sticky failure and
// returns zeros, so a parser can read every field and check ok() once.
class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0), m_ok(true) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    float f32();
    void str(std::string& out);

    bool ok() const { return m_ok; }
    // True when every byte was consumed and no read failed.
    bool done() const { return m_ok && m_pos == m_size; }

private:
    bool need(size_t n);

    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
    bool m_ok;
};

struct Frame
{
    uint16_t type;
    std::vector<uint8_t> payload;
};

// Splits a byte stream into frames. The buffer never holds more than one maximum frame plus one
// read chunk, because next() is called after every feed().
class FrameDecoder
{
public:
    enum Result { NeedMore, GotFrame, Oversize, BadReserved };

    FrameDecoder() : m_pos(0) {}

    void feed(const uint8_t* data, size_t size);
    // Extracts the next complete frame. Oversize and BadReserved are terminal for the stream.
    Result next(Frame& out);
    void reset() { m_buf.clear(); m_pos = 0; }
    size_t buffered() const { return m_buf.size() - m_pos; }

private:
    std::vector<uint8_t> m_buf;
    size_t m_pos;
};

struct HelloMsg        { uint16_t version; std::string name; };
struct CreateBotMsg    { uint32_t requestId; std::string name; uint8_t team; };
struct EnterVehicleMsg { uint32_t requestId; uint8_t playerId; uint32_t vehicleId; uint8_t seat; };
struct RemoveBotMsg    { uint32_t requestId; uint8_t playerId; };

// Axis order on the wire: cyclic pitch, cyclic roll, collective, pedals.
enum Axis { AxisPitch = 0, AxisRoll = 1, AxisCollective = 2, AxisPedals = 3, AxisCount = 4 };

struct SetInputMsg
{
    uint8_t playerId;
    uint32_t tick;
    float axes[AxisCount];
    uint32_t buttons;
};

// Each parser requires the payload to be exactly the message, with no trailing bytes.
bool parseHello(const std::vector<uint8_t>& p, HelloMsg& out);
bool parseCreateBot(const std::vector<uint8_t>& p, CreateBotMsg& out);
bool parseEnterVehicle(const std::vector<uint8_t>& p, EnterVehicleMsg& out);
bool parseRemoveBot(const std::vector<uint8_t>& p, RemoveBotMsg& out);
bool parseSetInput(const std::vector<uint8_t>& p, SetInputMsg& out);

// True for the message types a sidecar may send.
bool isClientMessage(uint16_t type);

void encodeWelcome(std::vector<uint8_t>& out, float tickRate, uint32_t tick, const std::string& map,
                   const std::string& mode);
void encodeBotCreated(std::vector<uint8_t>& out, uint32_t requestId, uint8_t playerId);
void encodeAck(std::vector<uint8_t>& out, uint32_t requestId);
void encodeBotRemoved(std::vector<uint8_t>& out, uint8_t playerId, uint8_t reason);
// Messages longer than 1024 bytes are truncated.
void encodeError(std::vector<uint8_t>& out, uint32_t requestId, uint16_t code, const std::string& message);

struct StateEntry
{
    uint8_t playerId;
    uint8_t flags;
    uint8_t team;
    uint8_t seat;
    uint32_t rootId;
    float pos[3];
    float ypr[3];
    float vel[3];
    float angVel[3];
    float health;
};

// At most 255 entries are written; the rest are dropped.
void encodeTickState(std::vector<uint8_t>& out, uint32_t tick, double time, const std::vector<StateEntry>& entries);

// The engine's convertAxisToInternalResolution (Game/Common/PlayerAction.cpp).
float convertAxisToInternalResolution(float value);

// What a human's axis value becomes on the server: PlayerAction::setAxis stores
// short(int(convertAxisToInternalResolution(v) * 100)) and PlayerAction::getAxis yields that / 100.
float quantizeLikeHuman(float value);

// Validates and conditions one SetInput axis: rejects NaN and infinity, clamps to -1..1, then
// quantizes like a human's input. Returns false for a rejected value and leaves out unchanged.
bool conditionAxis(float value, float& out);

} // namespace proto
} // namespace serverext

#endif // SERVEREXT_PROTOCOL_H
