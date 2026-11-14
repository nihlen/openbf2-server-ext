// The script host of the admin channel (docs/admin-channel-draft.md "Scripts"): WebAdmin pushes
// JavaScript source, and the module runs it on the main thread in an embedded QuickJS-NG
// (third_party/quickjs-ng), one runtime per script. Engine-free: a script reaches the game only
// through World, which Admin.cpp implements with the engine and the unit tests with a mock.
//
// Everything here runs on the server's main thread. No call blocks, and no script error, bad
// argument or hostile source may crash the caller: every entry into a script is bounded by a memory
// limit, a stack limit and a CPU-time budget.

#ifndef SERVEREXT_SCRIPT_H
#define SERVEREXT_SCRIPT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace serverext {
namespace script {

const uint8_t kApiVersion = 1;
const size_t kMaxScripts = 8;
const size_t kMaxNameBytes = 64;
const size_t kMaxSourceBytes = 60000;
const size_t kMaxPayloadBytes = 8192;          // ScriptEvent payload, ScriptMessage message, error text
const size_t kMaxResultBytes = 60000;          // finish()'s JSON text
const size_t kMaxSayBytes = 1024;              // game.say text, as the Say command
const size_t kMemoryLimit = 4 * 1024 * 1024;   // per script
const size_t kStackLimit = 256 * 1024;
const uint64_t kTopLevelBudgetUs = 50000;      // the source's top-level code
const uint64_t kCallBudgetUs = 5000;           // any other call into a script
const uint64_t kWindowBudgetUs = 30000;        // summed over kWindowTicks consecutive ticks
const int kWindowTicks = 30;
const int kMaxEventsPerTick = 16;              // ScriptEvents of one script in one tick
const float kMaxSpeed = 1000.0f;               // m/s, per axis
const float kMaxAngularSpeed = 100.0f;         // rad/s, per axis

// ScriptFinished reasons (the wire's).
enum Reason : uint8_t
{
    ReasonFinished = 1, ReasonStopped = 2, ReasonError = 3, ReasonBudget = 4, ReasonLevelUnload = 5
};

// ScriptEvent kinds (the wire's).
enum EventKind : uint8_t { EventData = 1, EventLog = 2 };

// The admin Error codes the host answers with (admin::ErrorCode values; 0 is success).
enum Result : uint16_t { Ok = 0, Unavailable = 5, Refused = 6, InvalidValue = 7 };

// What game.player(id) returns.
struct PlayerView
{
    int id;
    std::string name;       // UTF-8
    int team;
    bool alive, bot, seated;
    int seat;               // the PCO id, -1 on foot or dead
    uint32_t rootId;        // the root object's networkable id, 0 without one
    std::string vehicle;    // the root object's template, empty without a root object
    bool hasRoot;           // false when dead: position, rotation and velocity are null
    float pos[3], rot[3], vel[3];
    PlayerView();
};

// The game as a script sees it, and where its output goes. All calls come on the main thread,
// from inside Host's entry points.
class World
{
public:
    virtual void players(std::vector<int>& ids) = 0;
    // @return False for no such player.
    virtual bool player(int id, PlayerView& out) = 0;
    // The Teleport command on the player's root object; pos or rot may be null to keep it.
    // @return False when the player has no root object.
    virtual bool teleport(int id, const float* pos, const float* rot) = 0;
    // @return False without a root object or a physics node.
    virtual bool setVelocity(int id, const float v[3]) = 0;
    virtual bool setAngularVelocity(int id, const float v[3]) = 0;
    // To everyone (player -1) or to one player; text is UTF-8 without control bytes, at most kMaxSayBytes.
    virtual void say(const std::string& text, int player) = 0;
    virtual std::string map() = 0;
    virtual uint32_t tick() = 0;
    virtual double time() = 0;
    // CPU time of the calling thread in microseconds (CLOCK_THREAD_CPUTIME_ID).
    virtual uint64_t cpuMicros() = 0;
    // A line for the module's log: script.log, and the host's own notes about a script.
    virtual void log(uint32_t instance, const std::string& name, const std::string& line) = 0;
    virtual void scriptEvent(uint32_t instance, uint8_t kind, const std::string& payload) = 0;
    virtual void scriptFinished(uint32_t instance, const std::string& name, uint8_t reason, const std::string& result,
                                uint32_t calls, uint32_t totalUs, uint32_t maxUs) = 0;

protected:
    ~World() {}
};

// The thread CPU clock World::cpuMicros is meant to return.
uint64_t threadCpuMicros();

// True for 1..kMaxNameBytes printable ASCII bytes (0x20..0x7E).
bool validName(const std::string& name);

struct Stats
{
    uint64_t calls;         // entries into scripts since the last takeStats
    uint64_t totalUs;       // their CPU time
    uint64_t maxTickUs;     // the most CPU time all scripts took in one tick()
    uint64_t logsDropped;   // script.log lines over the per-tick limit
    Stats() : calls(0), totalUs(0), maxTickUs(0), logsDropped(0) {}
};

class Script;

class Host
{
public:
    explicit Host(World* world);
    ~Host();

    // StartScript. Runs the top-level code. A script that ended there (finish, or reason 3/4 is an
    // error here) is reported by settle().
    // @return Ok, or the Error code with error set: Unavailable (the API version is newer than
    //         kApiVersion), Refused (kMaxScripts run, or the instance is in use), InvalidValue (name,
    //         arguments that are not JSON, source that does not compile or whose top-level code throws).
    uint16_t start(uint32_t instance, uint8_t apiVersion, const std::string& name, const std::string& argsJson,
                   const std::string& source, std::string& error);
    // StopScript: marks the script to end with reason 2; settle() ends it.
    // @return Ok or Unavailable (no such script).
    uint16_t stop(uint32_t instance, std::string& error);
    // ScriptMessage: runs the message handlers with the parsed JSON.
    // @return Ok, Unavailable (no such script) or InvalidValue (not JSON, or too long).
    uint16_t message(uint32_t instance, const std::string& json, std::string& error);
    // Ends the scripts that are due to end (stop handlers, then ScriptFinished) and hands the queued
    // ScriptEvents and ScriptFinished to the world, in order. The caller calls it after it has queued
    // its reply to start(), stop() or message(), so the reply comes first.
    void settle();
    // Once per module tick: every script's tick handlers, scripts in start order, then settle().
    void tick();
    // The level unloads: every script ends with reason 5 (stop handlers "level unload").
    void levelUnload();
    // The link to WebAdmin closed: every script's stop handlers run ("link closed") and the scripts
    // are freed; nothing is reported.
    void linkClosed();

    void list(std::vector<std::pair<uint32_t, std::string> >& out) const;
    size_t count() const { return m_scripts.size(); }
    // The counters since the last call.
    Stats takeStats();

private:
    friend class Script;
    struct Outbound
    {
        bool finished;
        uint32_t instance;
        std::string name;
        uint8_t code;          // the reason, or the event kind
        std::string text;      // the result, or the payload
        uint32_t calls, totalUs, maxUs;
    };

    Script* find(uint32_t instance) const;
    void endDue(bool report);
    void flush();

    World* m_world;
    std::vector<Script*> m_scripts;   // in start order
    std::vector<Outbound> m_outbox;
    Stats m_stats;
    bool m_inTick;
    uint64_t m_tickUs;

    Host(const Host&);
    Host& operator=(const Host&);
};

} // namespace script
} // namespace serverext

#endif // SERVEREXT_SCRIPT_H
