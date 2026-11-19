// Unit tests for the script host (src/Script.cpp) against a mock world: the life cycle and every
// end reason, the limits, what the language has and has not, and the host API. No engine.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <map>
#include <string>
#include <vector>

#include "Script.h"

using namespace serverext;
using namespace serverext::script;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct Finished
{
    uint32_t instance;
    std::string name;
    uint8_t reason;
    std::string result;
    uint32_t calls, totalUs, maxUs;
};

struct Event
{
    uint32_t instance;
    uint8_t kind;
    std::string payload;
};

struct Teleport
{
    int id;
    bool hasPos, hasRot;
    float pos[3], rot[3];
};

struct Speed
{
    int id;
    bool angular;
    float v[3];
};

class MockWorld : public World
{
public:
    MockWorld() : tickNumber(100), fakeClock(false), fakeNow(0), fakeStep(0) {}

    std::map<int, PlayerView> all;
    std::vector<Teleport> teleports;
    std::vector<Speed> speeds;
    std::vector<std::pair<std::string, int> > says;
    std::vector<std::string> logs;
    std::vector<Event> events;
    std::vector<Finished> finished;
    uint32_t tickNumber;
    bool fakeClock;        // a fake CPU clock that every game.say of a script advances by fakeStep (the budget
                           // check reads the clock at times the tests cannot count, so reading never advances it)
    uint64_t fakeNow, fakeStep;

    virtual void players(std::vector<int>& ids)
    {
        for (std::map<int, PlayerView>::iterator it = all.begin(); it != all.end(); ++it)
            ids.push_back(it->first);
    }
    virtual bool player(int id, PlayerView& out)
    {
        std::map<int, PlayerView>::iterator it = all.find(id);
        if (it == all.end())
            return false;
        out = it->second;
        return true;
    }
    virtual bool teleport(int id, const float* pos, const float* rot)
    {
        if (!all.count(id) || !all[id].hasRoot)
            return false;
        Teleport t;
        t.id = id;
        t.hasPos = pos != 0;
        t.hasRot = rot != 0;
        for (int i = 0; i < 3; ++i)
        {
            t.pos[i] = pos ? pos[i] : 0.0f;
            t.rot[i] = rot ? rot[i] : 0.0f;
        }
        teleports.push_back(t);
        return true;
    }
    bool speed(int id, const float v[3], bool angular)
    {
        if (!all.count(id) || !all[id].hasRoot)
            return false;
        Speed s;
        s.id = id;
        s.angular = angular;
        for (int i = 0; i < 3; ++i)
            s.v[i] = v[i];
        speeds.push_back(s);
        return true;
    }
    virtual bool setVelocity(int id, const float v[3]) { return speed(id, v, false); }
    virtual bool setAngularVelocity(int id, const float v[3]) { return speed(id, v, true); }

    // API 2: objects by networkable id. Teleports and speeds of objects are recorded with id = -netId.
    std::map<uint32_t, ObjectView> objects;
    virtual bool object(uint32_t netId, ObjectView& out)
    {
        std::map<uint32_t, ObjectView>::iterator it = objects.find(netId);
        if (it == objects.end())
            return false;
        out = it->second;
        return true;
    }
    virtual bool teleportObject(uint32_t netId, const float* pos, const float* rot)
    {
        if (!objects.count(netId))
            return false;
        Teleport t;
        t.id = -static_cast<int>(netId);
        t.hasPos = pos != 0;
        t.hasRot = rot != 0;
        for (int i = 0; i < 3; ++i)
        {
            t.pos[i] = pos ? pos[i] : 0.0f;
            t.rot[i] = rot ? rot[i] : 0.0f;
        }
        teleports.push_back(t);
        return true;
    }
    bool objectSpeed(uint32_t netId, const float v[3], bool angular)
    {
        if (!objects.count(netId))
            return false;
        Speed s;
        s.id = -static_cast<int>(netId);
        s.angular = angular;
        for (int i = 0; i < 3; ++i)
            s.v[i] = v[i];
        speeds.push_back(s);
        return true;
    }
    virtual bool setObjectVelocity(uint32_t netId, const float v[3]) { return objectSpeed(netId, v, false); }
    virtual bool setObjectAngularVelocity(uint32_t netId, const float v[3]) { return objectSpeed(netId, v, true); }
    std::vector<uint32_t> activated;
    virtual bool activateProjectile(uint32_t netId)
    {
        if (!objects.count(netId) || objects[netId].templ.find("hellfire") == std::string::npos)
            return false;
        activated.push_back(netId);
        return true;
    }
    virtual void say(const std::string& text, int player)
    {
        says.push_back(std::make_pair(text, player));
        if (fakeClock)
            fakeNow += fakeStep;
    }
    virtual std::string map() { return "dalian_2_v_2"; }
    virtual uint32_t tick() { return tickNumber; }
    virtual double time() { return tickNumber / 30.0; }
    virtual uint64_t cpuMicros()
    {
        return fakeClock ? fakeNow : threadCpuMicros();
    }
    virtual void log(uint32_t instance, const std::string& name, const std::string& line)
    {
        char head[96];
        std::snprintf(head, sizeof(head), "%s(%u): ", name.c_str(), instance);
        logs.push_back(head + line);
    }
    virtual void scriptEvent(uint32_t instance, uint8_t kind, const std::string& payload)
    {
        Event e;
        e.instance = instance;
        e.kind = kind;
        e.payload = payload;
        events.push_back(e);
    }
    virtual void scriptFinished(uint32_t instance, const std::string& name, uint8_t reason, const std::string& result,
                                uint32_t calls, uint32_t totalUs, uint32_t maxUs)
    {
        Finished f;
        f.instance = instance;
        f.name = name;
        f.reason = reason;
        f.result = result;
        f.calls = calls;
        f.totalUs = totalUs;
        f.maxUs = maxUs;
        finished.push_back(f);
    }

    void addPlayer(int id, const char* name, int team, bool alive)
    {
        PlayerView p;
        p.id = id;
        p.name = name;
        p.team = team;
        p.alive = alive;
        p.hasRoot = alive;
        all[id] = p;
    }
};

// A host with its world; start() checks the result and settles, as Admin.cpp does after its reply.
struct Fixture
{
    MockWorld world;
    Host host;
    std::string error;
    Fixture() : host(&world) {}

    uint16_t start(uint32_t instance, const std::string& source, const std::string& args = "", const char* name = "test")
    {
        error.clear();
        const uint16_t r = host.start(instance, 1, name, args, source, error);
        host.settle();
        return r;
    }
    void tick(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            ++world.tickNumber;
            host.tick();
        }
    }
    // The data payloads emitted so far, joined with '|'.
    std::string data() const
    {
        std::string out;
        for (size_t i = 0; i < world.events.size(); ++i)
            if (world.events[i].kind == EventData)
                out += (out.empty() ? "" : "|") + world.events[i].payload;
        return out;
    }
};

bool validUtf8(const std::string& s)
{
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (!len || i + len > s.size())
            return false;
        if (len == 3 && c == 0xED && static_cast<unsigned char>(s[i + 1]) >= 0xA0)
            return false;   // a surrogate
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

double wallMs(const std::chrono::steady_clock::time_point& t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- life cycle

void testLifeCycle()
{
    Fixture f;
    CHECK(f.start(7, "let n = 0;\n"
                     "script.onTick(t => { n++; script.emit([n, t, game.tick]); });\n"
                     "script.onStop(reason => script.emit('stop:' + reason));\n") == Ok);
    CHECK(f.host.count() == 1);
    CHECK(f.world.events.empty() && f.world.finished.empty());
    f.tick(2);
    CHECK(f.data() == "[1,101,101]|[2,102,102]");
    std::vector<std::pair<uint32_t, std::string> > list;
    f.host.list(list);
    CHECK(list.size() == 1 && list[0].first == 7 && list[0].second == "test");

    // StopScript: nothing happens until settle (Admin.cpp replies Ok in between).
    std::string error;
    CHECK(f.host.stop(7, error) == Ok);
    CHECK(f.world.finished.empty() && f.host.count() == 1);
    f.host.settle();
    CHECK(f.host.count() == 0);
    CHECK(f.world.finished.size() == 1);
    if (f.world.finished.size() == 1)
    {
        const Finished& e = f.world.finished[0];
        CHECK(e.instance == 7 && e.name == "test" && e.reason == ReasonStopped && e.result.empty());
        CHECK(e.calls == 4);   // top level, two ticks, the stop handlers
    }
    CHECK(f.world.events.size() == 3 && f.world.events[2].payload == "\"stop:stopped\"");
    CHECK(f.host.stop(7, error) == Unavailable && error == "no such script");
    f.tick(3);
    CHECK(f.world.events.size() == 3);
    const Stats stats = f.host.takeStats();
    CHECK(stats.calls == 4 && f.host.takeStats().calls == 0);
}

void testFinish()
{
    {
        Fixture f;
        CHECK(f.start(1, "let n = 0;\n"
                         "script.onStop(r => script.emit(r));\n"
                         "script.onTick(() => { if (++n === 3) { script.finish({ ticks: n }); script.finish('second'); } });\n"
                         "script.onTick(() => script.emit('second handler ' + n));\n") == Ok);
        f.tick(5);
        CHECK(f.host.count() == 0);
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonFinished &&
              f.world.finished[0].result == "{\"ticks\":3}");
        // The handler that called finish was the last to run; then the stop handlers, with the reason.
        CHECK(f.data() == "\"second handler 1\"|\"second handler 2\"|\"finished\"");
    }
    {
        // finish() in the top-level code: start succeeds, and the end is reported by settle.
        Fixture f;
        CHECK(f.host.start(2, 1, "once", "", "script.emit('hi'); script.finish();", f.error) == Ok);
        CHECK(f.world.events.empty() && f.world.finished.empty());   // the Ok reply comes first
        f.host.settle();
        CHECK(f.world.events.size() == 1 && f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonFinished &&
              f.world.finished[0].result.empty() && f.world.finished[0].name == "once");
        CHECK(f.host.count() == 0);
    }
    {
        // An error after finish() in the same handler does not turn the end into an error.
        Fixture f;
        CHECK(f.start(3, "script.onTick(() => { script.finish(1); throw new Error('late'); });") == Ok);
        f.tick();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonFinished && f.world.finished[0].result == "1");
    }
    {
        // A result that is not JSON-serialisable, or too long, throws in the script.
        Fixture f;
        CHECK(f.start(4, "script.onTick(() => {\n"
                         "  try { script.finish(() => 1); } catch (e) { script.emit(e.name); }\n"
                         "  try { const a = {}; a.a = a; script.finish(a); } catch (e) { script.emit(e.name); }\n"
                         "  try { script.finish('x'.repeat(60001)); } catch (e) { script.emit(e.name); }\n"
                         "});") == Ok);
        f.tick();
        CHECK(f.data() == "\"TypeError\"|\"TypeError\"|\"RangeError\"");
        CHECK(f.host.count() == 1);
    }
}

void testArgumentsAndMessages()
{
    Fixture f;
    CHECK(f.start(1, "script.emit(script.args);\n"
                     "script.onMessage(m => script.emit({ got: m, altitude: m.altitude }));",
                  "{\"altitude\":300,\"players\":[3,5]}") == Ok);
    CHECK(f.data() == "{\"altitude\":300,\"players\":[3,5]}");
    std::string error;
    CHECK(f.host.message(1, "{\"altitude\":250}", error) == Ok);
    f.host.settle();
    CHECK(f.world.events.size() == 2 && f.world.events[1].payload == "{\"got\":{\"altitude\":250},\"altitude\":250}");
    CHECK(f.host.message(1, "{altitude:250}", error) == InvalidValue && has(error, "not JSON"));
    CHECK(f.host.message(2, "1", error) == Unavailable);
    CHECK(f.host.message(1, "\"" + std::string(kMaxPayloadBytes, 'x') + "\"", error) == InvalidValue);
    CHECK(f.host.message(1, std::string(4000, '[') + std::string(4000, ']'), error) == InvalidValue ||
          f.host.count() == 1);   // deep nesting: an error or a parsed value, never a crash
    CHECK(f.host.count() == 1);

    Fixture g;
    CHECK(g.start(1, "script.emit(script.args === null);") == Ok);
    CHECK(g.data() == "true");
    CHECK(g.start(2, "script.emit(1);", "{nope}") == InvalidValue && has(g.error, "not JSON"));
    CHECK(g.start(3, "script.emit(1);", std::string(20000, '[')) == InvalidValue);
    CHECK(g.host.count() == 1);
}

void testStartErrors()
{
    Fixture f;
    CHECK(f.start(1, "let a = 1;\nlet b = ;\n") == InvalidValue);
    CHECK(has(f.error, "SyntaxError") && has(f.error, "test:2:"));
    std::printf("  syntax error: %s\n", f.error.c_str());
    CHECK(f.start(1, "function boom() {\n  throw new Error('at top');\n}\nboom();\n") == InvalidValue);
    CHECK(has(f.error, "Error: at top") && has(f.error, "boom") && has(f.error, "test:2:"));
    CHECK(f.host.count() == 0 && f.world.finished.empty());

    // What a failed start emitted is not sent.
    CHECK(f.start(1, "script.emit('lost'); throw 1;") == InvalidValue);
    CHECK(f.world.events.empty());

    CHECK(f.host.start(1, 3, "test", "", "1", f.error) == Unavailable && has(f.error, "script API 3"));
    CHECK(f.host.start(1, 2, "test", "", "1", f.error) == Ok);
    f.host.settle();
    CHECK(f.host.stop(1, f.error) == Ok);
    f.host.settle();
    CHECK(f.host.start(1, 0, "test", "", "1", f.error) == InvalidValue);
    CHECK(f.host.start(0, 1, "test", "", "1", f.error) == InvalidValue);
    CHECK(f.host.start(1, 1, "", "", "1", f.error) == InvalidValue);
    CHECK(f.host.start(1, 1, std::string(65, 'n'), "", "1", f.error) == InvalidValue);
    CHECK(f.host.start(1, 1, "bad\nname", "", "1", f.error) == InvalidValue);
    CHECK(f.host.start(1, 1, "test", "", "", f.error) == InvalidValue);
    CHECK(f.host.start(1, 1, "test", "", std::string(kMaxSourceBytes + 1, ' '), f.error) == InvalidValue);
    CHECK(f.host.count() == 0);

    // Eight scripts at most, and an instance only once.
    for (uint32_t i = 1; i <= 8; ++i)
        CHECK(f.start(i, "script.onTick(() => {});") == Ok);
    CHECK(f.start(9, "1") == Refused && has(f.error, "8 scripts"));
    std::string error;
    CHECK(f.host.stop(8, error) == Ok);
    f.host.settle();
    CHECK(f.start(3, "1") == Refused && has(f.error, "in use"));
    CHECK(f.start(9, "1") == Ok);
    CHECK(f.host.count() == 8);
}

// ---------------------------------------------------------------- errors

void testErrors()
{
    {
        Fixture f;
        CHECK(f.start(1, "function boom() { throw new Error('boom'); }\n"
                         "script.onStop(() => script.emit('never'));\n"
                         "script.onTick(() => boom());") == Ok);
        f.tick();
        CHECK(f.host.count() == 0 && f.world.finished.size() == 1);
        if (!f.world.finished.empty())
        {
            const Finished& e = f.world.finished[0];
            CHECK(e.reason == ReasonError && has(e.result, "Error: boom") && has(e.result, "at boom") && has(e.result, "test:1:"));
        }
        CHECK(f.world.events.empty());   // no stop handlers after an error
    }
    {
        // A thrown value that is not an Error.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { throw { code: 5 }; });") == Ok);
        f.tick();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonError && !f.world.finished[0].result.empty());
    }
    {
        // An unhandled rejection is an error; a handled one is not; async handlers work.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { Promise.reject(new Error('nope')); });") == Ok);
        CHECK(f.start(2, "script.onTick(() => { Promise.reject(1).catch(() => {}); });") == Ok);
        CHECK(f.start(3, "script.onTick(async t => { await null; const x = await Promise.resolve(t + 1); script.emit(x); });") == Ok);
        CHECK(f.start(4, "script.onTick(async () => { await null; throw new Error('async boom'); });") == Ok);
        f.tick(2);
        CHECK(f.world.finished.size() == 2);
        if (f.world.finished.size() == 2)
        {
            CHECK(f.world.finished[0].instance == 1 && f.world.finished[0].reason == ReasonError &&
                  has(f.world.finished[0].result, "unhandled promise rejection") && has(f.world.finished[0].result, "nope"));
            CHECK(f.world.finished[1].instance == 4 && f.world.finished[1].reason == ReasonError &&
                  has(f.world.finished[1].result, "async boom"));
        }
        CHECK(f.data() == "102|103");
        CHECK(f.host.count() == 2);
    }
    {
        // A script that catches its errors keeps running.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { try { null.x; } catch (e) { script.emit(e.name); } });") == Ok);
        f.tick(2);
        CHECK(f.data() == "\"TypeError\"|\"TypeError\"" && f.host.count() == 1);
    }
    {
        // Errors in stop handlers are logged and ignored; every stop handler runs.
        Fixture f;
        CHECK(f.start(1, "script.onStop(() => { throw new Error('in stop'); });\n"
                         "script.onStop(r => { script.finish('ignored'); script.emit('second ' + r); game.say('bye'); });") == Ok);
        std::string error;
        CHECK(f.host.stop(1, error) == Ok);
        f.host.settle();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonStopped && f.world.finished[0].result.empty());
        CHECK(f.data() == "\"second stopped\"");
        CHECK(f.world.logs.size() == 1 && has(f.world.logs[0], "stop handler") && has(f.world.logs[0], "in stop"));
        CHECK(f.world.says.size() == 1 && f.world.says[0].first == "bye");
    }
}

// ---------------------------------------------------------------- limits

void testTimeBudget()
{
    {
        // An endless loop in a tick handler: one long tick, then reason 4.
        Fixture f;
        CHECK(f.start(1, "script.onStop(() => script.emit('never'));\n"
                         "script.onTick(() => { for (;;) {} });") == Ok);
        CHECK(f.start(2, "script.onTick(t => script.emit(t));") == Ok);
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        f.tick();
        const double ms = wallMs(t0);
        CHECK(ms < 1000.0);
        CHECK(f.world.finished.size() == 1);
        if (!f.world.finished.empty())
        {
            const Finished& e = f.world.finished[0];
            CHECK(e.instance == 1 && e.reason == ReasonBudget && has(e.result, "over the time budget") &&
                  has(e.result, "limit 5 ms") && has(e.result, "interrupted"));
            CHECK(e.maxUs >= kCallBudgetUs && e.maxUs < 500000);
            std::printf("  endless loop ended after %.1f ms of wall time (%u us of CPU)\n", ms, e.maxUs);
        }
        CHECK(f.data() == "101");   // the other script ran, and no stop handler of the first
        f.tick();
        CHECK(f.host.count() == 1 && f.data() == "101|102");
    }
    {
        // An endless loop that swallows exceptions: the interruption cannot be caught.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { for (;;) { try { for (;;) {} } catch (e) {} } });") == Ok);
        f.tick();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonBudget);
    }
    {
        // An endless chain of promise jobs.
        Fixture f;
        CHECK(f.start(1, "function again() { Promise.resolve().then(again); }\nscript.onTick(again);") == Ok);
        f.tick();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonBudget);
    }
    {
        // The budget is checked where QuickJS polls for interrupts: at loop back-edges and calls of
        // script code, once per JS_INTERRUPT_COUNTER_INIT of them (100 in this build, CMakeLists.txt).
        // A loop whose every round spends its time inside one built-in is therefore ended a little
        // later than a loop of script code (printed, and bounded here for two built-ins that take a
        // few microseconds a round).
        const struct { const char* what; const char* body; } loops[] = {
            { "JSON.stringify of 40 numbers", "JSON.stringify(data);" },
            { "a 4 KiB typed-array fill", "bytes.fill(1);" },
        };
        for (size_t i = 0; i < sizeof(loops) / sizeof(loops[0]); ++i)
        {
            Fixture f;
            const std::string source = std::string(
                "const data = Array.from({ length: 40 }, (_, i) => i * 1.5); const bytes = new Uint8Array(4096);\n"
                "script.onTick(() => { for (;;) { ") + loops[i].body + " } });";
            CHECK(f.start(1, source) == Ok);
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            f.tick();
            const double ms = wallMs(t0);
            CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonBudget && ms < 3000.0);
            std::printf("  endless loop around %s ended after %.1f ms\n", loops[i].what, ms);
        }
    }
    {
        // One built-in call is never interrupted; what it can cost is bounded by the memory limit,
        // which a large call runs into first. Measured for arrays of numbers that are sorted (as
        // strings: the default order), joined and written as JSON, each in a single call (printed).
        const int sizes[] = { 5000, 20000, 100000 };
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        {
            Fixture f;
            char source[320];
            std::snprintf(source, sizeof(source),
                          "const big = Array.from({ length: %d }, (_, i) => (i * 7919) %% 100003);\n"
                          "script.onTick(() => { big.sort(); big.join(','); JSON.stringify(big); });", sizes[i]);
            CHECK(f.start(1, source) == Ok);
            const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            f.tick();
            const double ms = wallMs(t0);
            CHECK(ms < 2000.0);
            std::printf("  sort, join and JSON.stringify of %d numbers in one tick: %.1f ms, %s\n", sizes[i], ms,
                        f.world.finished.empty() ? "the script goes on"
                                                 : f.world.finished[0].result.substr(0, 60).c_str());
        }
    }
    {
        // The top-level code has 50 ms; running over it is a failed start.
        Fixture f;
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        CHECK(f.start(1, "for (;;) {}") == InvalidValue);
        CHECK(has(f.error, "over the time budget") && has(f.error, "limit 50 ms"));
        CHECK(wallMs(t0) < 2000.0 && f.host.count() == 0 && f.world.finished.empty());
    }
    {
        // 30 ms over 30 ticks: 2 ms per tick is within the call limit and over the average.
        Fixture f;
        f.world.fakeClock = true;
        f.world.fakeStep = 0;
        CHECK(f.start(1, "script.onTick(() => { game.say('work'); });") == Ok);
        f.world.fakeStep = 2000;
        f.tick(15);
        CHECK(f.host.count() == 1);   // 30 ms exactly
        f.tick();
        CHECK(f.host.count() == 0 && f.world.finished.size() == 1);
        if (!f.world.finished.empty())
        {
            const Finished& e = f.world.finished[0];
            CHECK(e.reason == ReasonBudget && has(e.result, "in 30 ticks") && e.calls == 17 && e.totalUs == 32000 &&
                  e.maxUs == 2000);
        }
    }
    {
        // 1 ms per tick never ends it, and time older than 30 ticks is forgotten.
        Fixture f;
        f.world.fakeClock = true;
        CHECK(f.start(1, "script.onTick(() => { game.say('work'); });") == Ok);
        f.world.fakeStep = 1000;
        f.tick(100);
        CHECK(f.host.count() == 1);
        f.world.fakeStep = 0;
        f.tick(30);
        f.world.fakeStep = 2900;
        f.tick(10);   // 29 ms
        f.world.fakeStep = 0;
        f.tick(30);
        f.world.fakeStep = 2900;
        f.tick(10);   // 29 ms again: the first 29 are out of the window
        CHECK(f.host.count() == 1);
        f.world.fakeStep = 4000;
        f.tick();     // 33 ms
        CHECK(f.host.count() == 0 && f.world.finished.size() == 1 && f.world.finished[0].calls == 182);
    }
    {
        // A slow stop handler is cut off and logged; the script still ends as stopped.
        Fixture f;
        CHECK(f.start(1, "script.onStop(() => { for (;;) {} });") == Ok);
        std::string error;
        f.host.stop(1, error);
        f.host.settle();
        CHECK(f.world.finished.size() == 1 && f.world.finished[0].reason == ReasonStopped);
        CHECK(!f.world.logs.empty() && has(f.world.logs.back(), "stop handler"));
    }
}

void testMemoryAndStack()
{
    {
        Fixture f;
        CHECK(f.start(1, "const keep = [];\n"
                         "script.onTick(() => { for (;;) keep.push(new Array(1000).fill(keep.length)); });") == Ok);
        f.tick();
        CHECK(f.host.count() == 0 && f.world.finished.size() == 1);
        if (!f.world.finished.empty())
        {
            CHECK(f.world.finished[0].reason == ReasonError && has(f.world.finished[0].result, "out of memory"));
            std::printf("  memory limit: %s\n", f.world.finished[0].result.substr(0, 60).c_str());
        }
    }
    {
        // One huge allocation. Running out of memory cannot be caught: the script ends.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { try { new Uint8Array(64 * 1024 * 1024); } catch (e) { script.emit(e.name); } });") == Ok);
        f.tick();
        CHECK(f.host.count() == 0 && f.world.events.empty() && f.world.finished.size() == 1 &&
              f.world.finished[0].reason == ReasonError && has(f.world.finished[0].result, "out of memory"));
    }
    {
        Fixture f;
        CHECK(f.start(1, "function deep(n) { return deep(n + 1) + 1; }\nscript.onTick(() => deep(0));") == Ok);
        f.tick();
        CHECK(f.world.finished.size() == 1);
        if (!f.world.finished.empty())
        {
            CHECK(f.world.finished[0].reason == ReasonError && has(f.world.finished[0].result, "stack"));
            std::printf("  stack limit: %s\n", f.world.finished[0].result.substr(0, 60).c_str());
        }
    }
    {
        // A memory-hungry top level is a failed start, and the next script starts fine.
        Fixture f;
        CHECK(f.start(1, "const a = []; for (;;) a.push('x'.repeat(100000) + a.length);") == InvalidValue);
        CHECK(f.start(2, "script.onTick(() => {});") == Ok);
    }
}

void testEventLimits()
{
    {
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => {\n"
                         "  for (let i = 0; i < 16; i++) script.emit(i);\n"
                         "  script.log('dropped');\n"
                         "  try { script.emit(16); } catch (e) { script.finish(e.name + ': ' + e.message); }\n"
                         "});") == Ok);
        f.tick();
        CHECK(f.world.events.size() == 16 && f.world.finished.size() == 1 && has(f.world.finished[0].result, "RangeError") &&
              has(f.world.finished[0].result, "16"));
        CHECK(f.world.logs.empty() && f.host.takeStats().logsDropped == 1);
    }
    {
        // The count starts again every tick.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { for (let i = 0; i < 16; i++) script.emit(i); });") == Ok);
        f.tick(3);
        CHECK(f.world.events.size() == 48 && f.host.count() == 1);
    }
    {
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => {\n"
                         "  script.emit('x'.repeat(8190));\n"
                         "  try { script.emit('x'.repeat(8191)); } catch (e) { script.emit(e.name); }\n"
                         "  try { script.emit(undefined); } catch (e) { script.emit(e.name); }\n"
                         "  try { script.emit(() => 1); } catch (e) { script.emit(e.name); }\n"
                         "});") == Ok);
        f.tick();
        CHECK(f.world.events.size() == 4 && f.world.events[0].payload.size() == 8192 &&
              f.world.events[1].payload == "\"RangeError\"" && f.world.events[2].payload == "\"TypeError\"" &&
              f.world.events[3].payload == "\"TypeError\"");
    }
    {
        // log: parts joined with spaces, to the module log and as an event; long lines are cut; lone
        // surrogates never reach the wire.
        Fixture f;
        CHECK(f.start(5, "script.log('altitude', 300, true, null, [1, 2], { a: 1 });\n"
                         "script.log('x'.repeat(9000));\n"
                         "script.log('bad \\ud800 surrogate', '\\u00e5\\u00e4\\u00f6');\n"
                         "script.emit('\\ud800');\n"
                         "script.log();", "", "nasa") == Ok);
        CHECK(f.world.logs.size() == 4 && f.world.logs[0] == "nasa(5): altitude 300 true null 1,2 [object Object]");
        CHECK(f.world.events.size() == 5 && f.world.events[0].kind == EventLog &&
              f.world.events[0].payload == "altitude 300 true null 1,2 [object Object]");
        CHECK(f.world.events[1].payload.size() == kMaxPayloadBytes);
        CHECK(validUtf8(f.world.events[2].payload) && has(f.world.events[2].payload, "\xC3\xA5\xC3\xA4\xC3\xB6"));
        CHECK(validUtf8(f.world.events[3].payload) && f.world.events[3].kind == EventData);
        CHECK(f.world.events[4].payload.empty());
    }
}

// ---------------------------------------------------------------- the language

void testLanguage()
{
    {
        Fixture f;
        CHECK(f.start(1,
            "const absent = ['Date', 'RegExp', 'Proxy', 'performance', 'WeakRef', 'FinalizationRegistry', 'setTimeout',\n"
            "  'setInterval', 'console', 'std', 'os', 'require', 'atob', 'btoa', 'DOMException', 'print', 'bjson'];\n"
            "script.emit(absent.filter(n => typeof globalThis[n] !== 'undefined'));\n"
            "const present = [typeof JSON, typeof Map, typeof Set, typeof Float32Array, typeof Promise, typeof Math,\n"
            "  typeof Symbol, typeof Reflect, typeof globalThis, typeof queueMicrotask];\n"
            "script.emit(present.filter(t => t === 'undefined').length);\n"
            "function tried(fn) { try { return 'ran: ' + fn(); } catch (e) { return e.name; } }\n"
            "script.emit([\n"
            "  tried(() => eval('1 + 1')),\n"
            "  tried(() => (0, eval)('1 + 1')),\n"
            "  tried(() => Function('return 1')()),\n"
            "  tried(() => new Function('return 1')()),\n"
            "  tried(() => (function () {}).constructor('return 1')()),\n"
            "  tried(() => (async function () {}).constructor('return 1')),\n"
            "  tried(() => (function* () {}).constructor('return 1')),\n"
            "]);\n"
            "// String methods that would need a RegExp must fail cleanly.\n"
            "script.emit([tried(() => 'a,b'.split(',').length), tried(() => 'aXb'.replace('X', '-')),\n"
            "  tried(() => 'aXbX'.replaceAll('X', '-')), tried(() => 'abc'.match('b')), tried(() => 'abc'.search('b')),\n"
            "  tried(() => [...'abc'.matchAll('b')].length), tried(() => 'abc'.includes('b'))]);\n"
            "// Strict mode: an undeclared name cannot be assigned.\n"
            "script.emit(tried(() => { undeclared = 1; }));\n"
            "script.emit(this === globalThis);\n") == Ok);
        CHECK(f.world.events.size() == 6);
        if (f.world.events.size() == 6)
        {
            if (f.world.events[0].payload != "[]")
                std::printf("  not absent: %s\n", f.world.events[0].payload.c_str());
            CHECK(f.world.events[0].payload == "[]");
            CHECK(f.world.events[1].payload == "0");
            std::printf("  eval and Function: %s\n", f.world.events[2].payload.c_str());
            CHECK(!has(f.world.events[2].payload, "ran"));
            std::printf("  string methods without RegExp: %s\n", f.world.events[3].payload.c_str());
            CHECK(has(f.world.events[3].payload, "\"ran: 2\",\"ran: a-b\",\"ran: a-b-\""));
            CHECK(f.world.events[4].payload == "\"ReferenceError\"");
            CHECK(f.world.events[5].payload == "true");
        }
    }
    {
        // A regular expression literal does not compile.
        Fixture f;
        CHECK(f.start(1, "const r = /a+/;\n") == InvalidValue && has(f.error, "SyntaxError"));
        std::printf("  regular expression literal: %s\n", f.error.substr(0, 70).c_str());
    }
    {
        // Modules are not loaded.
        Fixture f;
        CHECK(f.start(1, "import { x } from 'y';\n") == InvalidValue);
        CHECK(f.start(1, "script.onTick(async () => { await import('y'); });\n") == Ok ||
              f.host.count() == 0);
        f.tick();
        CHECK(f.host.count() == 0);   // either it never compiled, or the import failed
    }
    {
        // The globals are frozen and cannot be replaced.
        Fixture f;
        CHECK(f.start(1,
            "function tried(fn) { try { fn(); return 'ok'; } catch (e) { return e.name; } }\n"
            "script.emit([tried(() => { script.finish = 1; }), tried(() => { game.extra = 1; }),\n"
            "  tried(() => { globalThis.game = {}; }), tried(() => { delete globalThis.script; }),\n"
            "  tried(() => { game.tickRate = 60; }), Object.isFrozen(script), Object.isFrozen(game), game.tickRate]);\n") == Ok);
        CHECK(f.data() == "[\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",true,true,30]");
        CHECK(f.start(2, "const game = 1;") == InvalidValue);
        CHECK(f.start(3, "var script = 1;") == InvalidValue || f.host.count() == 1);
    }
    {
        // Each script has its own globals.
        Fixture f;
        CHECK(f.start(1, "globalThis.shared = 5; script.onTick(() => script.emit(typeof shared));") == Ok);
        CHECK(f.start(2, "script.onTick(() => script.emit(typeof shared));") == Ok);
        f.tick();
        CHECK(f.data() == "\"number\"|\"undefined\"");
    }
    {
        // Built-in loops over huge sparse arrays must not stall the server for long.
        Fixture f;
        CHECK(f.start(1, "const a = []; a.length = 4294967295;\n"
                         "script.onTick(() => { script.emit(a.indexOf(1)); });") == Ok);
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        f.tick();
        const double ms = wallMs(t0);
        std::printf("  indexOf over a 2^32 sparse array: %.1f ms, %s\n", ms,
                    f.world.finished.empty() ? "still running" : f.world.finished[0].result.substr(0, 50).c_str());
        CHECK(ms < 2000.0);
    }
}

// ---------------------------------------------------------------- the host API

void testHostApi()
{
    Fixture f;
    f.world.addPlayer(3, "Pilot \xC3\xA5", 2, true);
    PlayerView& p = f.world.all[3];
    p.bot = true; p.seated = true; p.seat = 1; p.rootId = 1819; p.vehicle = "ahe_ah1z";
    p.pos[0] = 10.5f; p.pos[1] = 150.0f; p.pos[2] = -20.0f;
    p.rot[0] = 90.0f; p.rot[1] = -5.0f; p.rot[2] = 1.5f;
    p.vel[0] = 1.0f; p.vel[1] = 2.0f; p.vel[2] = 3.0f;
    f.world.addPlayer(255, "dead", 1, false);

    CHECK(f.start(1,
        "script.emit([script.api, script.name, game.tickRate, game.map, game.tick, Math.round(game.time * 30)]);\n"
        "script.emit(game.players());\n"
        "script.emit(game.player(3));\n"
        "script.emit(game.player(255));\n"
        "script.emit([game.player(4), game.player(-1), game.player(256), game.player(1e9)]);\n"
        "script.emit([\n"
        "  game.teleport(3, [1, 2, 3], [10, 20, 30]),\n"
        "  game.teleport(3, [4, 5, 6], null),\n"
        "  game.teleport(3, null, [40, 50, 60]),\n"
        "  game.teleport(3, undefined, new Float32Array([1.5, 2.5, 3.5])),\n"
        "  game.teleport(255, [1, 2, 3], null),\n"
        "  game.teleport(4, [1, 2, 3], null),\n"
        "  game.teleport(300, [1, 2, 3], null),\n"
        "  game.setVelocity(3, [0, -3.5, 0]),\n"
        "  game.setAngularVelocity(3, [0, 6.25, 0]),\n"
        "  game.setVelocity(255, [0, 0, 0]),\n"
        "  game.setAngularVelocity(4, [0, 0, 0]),\n"
        "]);\n"
        "game.say('hello \\u00a7C1001all');\n"
        "game.say('to one', 3);\n"
        "game.say('line\\nbreak\\ttab\\u0000nul', undefined);\n"
        "game.say('', 3);\n"
        "game.say('nobody', 999);\n"
        "game.say('x'.repeat(2000));\n", "", "api") == Ok);
    CHECK(f.world.events.size() == 6);
    if (f.world.events.size() == 6)
    {
        CHECK(f.world.events[0].payload == "[2,\"api\",30,\"dalian_2_v_2\",100,100]");
        CHECK(f.world.events[1].payload == "[3,255]");
        CHECK(f.world.events[2].payload ==
              "{\"id\":3,\"name\":\"Pilot \xC3\xA5\",\"team\":2,\"alive\":true,\"bot\":true,\"seated\":true,\"seat\":1,"
              "\"rootId\":1819,\"vehicle\":\"ahe_ah1z\",\"position\":[10.5,150,-20],\"rotation\":[90,-5,1.5],"
              "\"velocity\":[1,2,3]}");
        CHECK(f.world.events[3].payload ==
              "{\"id\":255,\"name\":\"dead\",\"team\":1,\"alive\":false,\"bot\":false,\"seated\":false,\"seat\":-1,"
              "\"rootId\":0,\"vehicle\":\"\",\"position\":null,\"rotation\":null,\"velocity\":null}");
        CHECK(f.world.events[4].payload == "[null,null,null,null]");
        CHECK(f.world.events[5].payload == "[true,true,true,true,false,false,false,true,true,false,false]");
    }
    CHECK(f.world.teleports.size() == 4);
    if (f.world.teleports.size() == 4)
    {
        const Teleport& a = f.world.teleports[0];
        CHECK(a.id == 3 && a.hasPos && a.hasRot && a.pos[0] == 1.0f && a.pos[2] == 3.0f && a.rot[0] == 10.0f && a.rot[2] == 30.0f);
        CHECK(f.world.teleports[1].hasPos && !f.world.teleports[1].hasRot && f.world.teleports[1].pos[1] == 5.0f);
        CHECK(!f.world.teleports[2].hasPos && f.world.teleports[2].hasRot && f.world.teleports[2].rot[1] == 50.0f);
        CHECK(!f.world.teleports[3].hasPos && f.world.teleports[3].rot[2] == 3.5f);
    }
    CHECK(f.world.speeds.size() == 2);
    if (f.world.speeds.size() == 2)
    {
        CHECK(!f.world.speeds[0].angular && f.world.speeds[0].id == 3 && f.world.speeds[0].v[1] == -3.5f);
        CHECK(f.world.speeds[1].angular && f.world.speeds[1].v[1] == 6.25f);
    }
    CHECK(f.world.says.size() == 4);
    if (f.world.says.size() == 4)
    {
        CHECK(f.world.says[0].first == "hello \xC2\xA7" "C1001all" && f.world.says[0].second == -1);
        CHECK(f.world.says[1].first == "to one" && f.world.says[1].second == 3);
        CHECK(f.world.says[2].first == "linebreaktabnul" && f.world.says[2].second == -1);
        CHECK(f.world.says[3].first.size() == kMaxSayBytes);
    }

    // Wrong argument types throw a TypeError, bad numbers a RangeError.
    Fixture g;
    g.world.addPlayer(3, "p", 1, true);
    CHECK(g.start(1,
        "function tried(fn) { try { fn(); return 'ok'; } catch (e) { return e.name; } }\n"
        "script.emit([\n"
        "  tried(() => game.player('3')), tried(() => game.player()), tried(() => game.player(1.5)),\n"
        "  tried(() => game.player(NaN)), tried(() => game.teleport(3)), tried(() => game.teleport(3, null, null)),\n"
        "  tried(() => game.teleport('3', [1, 2, 3])), tried(() => game.teleport(3, 5)),\n"
        "  tried(() => game.teleport(3, [1, 2])), tried(() => game.teleport(3, [1, '2', 3])),\n"
        "  tried(() => game.setVelocity(3)), tried(() => game.setVelocity(3, 'fast')),\n"
        "  tried(() => game.setAngularVelocity(3, [1, 2])), tried(() => game.say(5)), tried(() => game.say('x', 'y')),\n"
        "  tried(() => script.onTick(5)), tried(() => script.onMessage()), tried(() => script.onStop('stop')),\n"
        "]);\n"
        "script.emit([\n"
        "  tried(() => game.teleport(3, [NaN, 0, 0])), tried(() => game.teleport(3, [0, Infinity, 0])),\n"
        "  tried(() => game.teleport(3, [9000, 0, 0])), tried(() => game.teleport(3, [0, -2000, 0])),\n"
        "  tried(() => game.teleport(3, null, [0, NaN, 0])), tried(() => game.setVelocity(3, [0, 1001, 0])),\n"
        "  tried(() => game.setVelocity(3, [0, -Infinity, 0])), tried(() => game.setAngularVelocity(3, [101, 0, 0])),\n"
        "  tried(() => game.setVelocity(3, [1000, -1000, 0])), tried(() => game.teleport(3, [8192, 10000, -8192])),\n"
        "]);\n") == Ok);
    CHECK(g.world.events.size() == 2);
    if (g.world.events.size() == 2)
    {
        std::string types = "[";
        for (int i = 0; i < 18; ++i)
            types += std::string(i ? "," : "") + "\"TypeError\"";
        types += "]";
        CHECK(g.world.events[0].payload == types);
        CHECK(g.world.events[1].payload ==
              "[\"RangeError\",\"RangeError\",\"RangeError\",\"RangeError\",\"RangeError\",\"RangeError\",\"RangeError\","
              "\"RangeError\",\"ok\",\"ok\"]");
    }
    CHECK(g.world.teleports.size() == 1 && g.world.speeds.size() == 1);
}

// API 2: objects by networkable id.
void testObjectApi()
{
    Fixture f;
    ObjectView heli;
    heli.id = 2065;
    heli.templ = "ahe_ah1z";
    heli.pos[0] = 1.5f; heli.pos[1] = 200.0f; heli.pos[2] = -3.0f;
    heli.rot[0] = 45.0f;
    heli.vel[1] = -9.5f;
    f.world.objects[2065] = heli;
    CHECK(f.start(1,
        "script.emit(game.object(2065));\n"
        "script.emit([game.object(2066), game.object(0), game.object(-1), game.object(65536), game.object(1e9)]);\n"
        "script.emit([\n"
        "  game.teleportObject(2065, [1, 2, 3], [10, 20, 30]),\n"
        "  game.teleportObject(2065, null, [40, 50, 60]),\n"
        "  game.teleportObject(2066, [1, 2, 3], null),\n"
        "  game.teleportObject(0, [1, 2, 3], null),\n"
        "  game.teleportObject(70000, [1, 2, 3], null),\n"
        "  game.setObjectVelocity(2065, [0, 2.5, 0]),\n"
        "  game.setObjectAngularVelocity(2065, [0, 0, 1.25]),\n"
        "  game.setObjectVelocity(2066, [0, 0, 0]),\n"
        "  game.setObjectAngularVelocity(0, [0, 0, 0]),\n"
        "]);\n"
        "function tried(fn) { try { fn(); return 'ok'; } catch (e) { return e.name; } }\n"
        "script.emit([\n"
        "  tried(() => game.object('2065')), tried(() => game.object()), tried(() => game.object(1.5)),\n"
        "  tried(() => game.teleportObject(2065)), tried(() => game.teleportObject(2065, null, null)),\n"
        "  tried(() => game.teleportObject(2065, [1, 2])), tried(() => game.setObjectVelocity(2065)),\n"
        "  tried(() => game.setObjectAngularVelocity('x', [0, 0, 0])),\n"
        "  tried(() => game.teleportObject(2065, [9000, 0, 0])), tried(() => game.teleportObject(2065, [0, NaN, 0])),\n"
        "  tried(() => game.setObjectVelocity(2065, [0, 1001, 0])), tried(() => game.setObjectAngularVelocity(2065, [0, 101, 0])),\n"
        "]);\n", "", "objects") == Ok);
    CHECK(f.world.events.size() == 4);
    if (f.world.events.size() == 4)
    {
        CHECK(f.world.events[0].payload ==
              "{\"id\":2065,\"template\":\"ahe_ah1z\",\"position\":[1.5,200,-3],\"rotation\":[45,0,0],\"velocity\":[0,-9.5,0]}");
        CHECK(f.world.events[1].payload == "[null,null,null,null,null]");
        CHECK(f.world.events[2].payload == "[true,true,false,false,false,true,true,false,false]");
        CHECK(f.world.events[3].payload ==
              "[\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\","
              "\"TypeError\",\"RangeError\",\"RangeError\",\"RangeError\",\"RangeError\"]");
    }
    CHECK(f.world.teleports.size() == 2);
    if (f.world.teleports.size() == 2)
    {
        const Teleport& a = f.world.teleports[0];
        CHECK(a.id == -2065 && a.hasPos && a.hasRot && a.pos[2] == 3.0f && a.rot[0] == 10.0f);
        CHECK(!f.world.teleports[1].hasPos && f.world.teleports[1].rot[2] == 60.0f);
    }
    CHECK(f.world.speeds.size() == 2);
    if (f.world.speeds.size() == 2)
    {
        CHECK(f.world.speeds[0].id == -2065 && !f.world.speeds[0].angular && f.world.speeds[0].v[1] == 2.5f);
        CHECK(f.world.speeds[1].angular && f.world.speeds[1].v[2] == 1.25f);
    }

    // game.activateProjectile: true only for a projectile; a bad id type throws.
    Fixture g;
    ObjectView missile;
    missile.id = 1852;
    missile.templ = "agm114_hellfire_tv";
    g.world.objects[1852] = missile;
    g.world.objects[2065] = heli;
    CHECK(g.start(1,
        "function tried(fn) { try { fn(); return 'ok'; } catch (e) { return e.name; } }\n"
        "script.emit([game.activateProjectile(1852), game.activateProjectile(2065), game.activateProjectile(0),\n"
        "  game.activateProjectile(70000), tried(() => game.activateProjectile('1852')), tried(() => game.activateProjectile())]);\n",
        "", "activate") == Ok);
    CHECK(g.world.events.size() == 1 && g.world.events[0].payload == "[true,false,false,false,\"TypeError\",\"TypeError\"]");
    CHECK(g.world.activated.size() == 1 && g.world.activated[0] == 1852);
}

// ---------------------------------------------------------------- handlers and order

void testHandlersAndOrder()
{
    {
        // Scripts tick in start order; a script's handlers in the order they were set, and one set
        // during a tick runs from that tick on.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => script.emit('a1'));\n"
                         "let added = false;\n"
                         "script.onTick(() => { script.emit('a2'); if (!added) { added = true; script.onTick(() => script.emit('a3')); } });") == Ok);
        CHECK(f.start(2, "script.onTick(() => script.emit('b'));") == Ok);
        f.tick(2);
        CHECK(f.data() == "\"a1\"|\"a2\"|\"a3\"|\"b\"|\"a1\"|\"a2\"|\"a3\"|\"b\"");
    }
    {
        // One script failing does not touch another; its ScriptFinished comes after what it emitted.
        Fixture f;
        CHECK(f.start(1, "script.onTick(() => { script.emit('before'); throw new Error('x'); });") == Ok);
        CHECK(f.start(2, "script.onTick(t => script.emit(t));") == Ok);
        f.tick(2);
        CHECK(f.data() == "\"before\"|101|102" && f.world.finished.size() == 1 && f.host.count() == 1);
    }
    {
        // A script without handlers stays until it is stopped.
        Fixture f;
        CHECK(f.start(1, "const x = 1;") == Ok);
        f.tick(40);
        CHECK(f.host.count() == 1 && f.world.finished.empty());
    }
}

void testLevelUnloadAndLinkClosed()
{
    {
        Fixture f;
        f.world.addPlayer(3, "p", 1, true);
        CHECK(f.start(1, "script.onStop(r => { script.emit(r); game.teleport(3, [1, 2, 3], null); });") == Ok);
        CHECK(f.start(2, "script.onTick(() => {});") == Ok);
        f.host.levelUnload();
        CHECK(f.host.count() == 0 && f.world.finished.size() == 2);
        if (f.world.finished.size() == 2)
            CHECK(f.world.finished[0].reason == ReasonLevelUnload && f.world.finished[0].result.empty() &&
                  f.world.finished[1].instance == 2 && f.world.finished[1].reason == ReasonLevelUnload);
        CHECK(f.data() == "\"level unload\"" && f.world.teleports.size() == 1);
    }
    {
        // A closing link: the stop handlers run, and nothing is reported.
        Fixture f;
        f.world.addPlayer(3, "p", 1, true);
        CHECK(f.start(1, "script.onStop(r => { script.emit(r); if (r === 'link closed') game.setVelocity(3, [0, 0, 0]); });") == Ok);
        CHECK(f.start(2, "script.onTick(() => { throw 1; });") == Ok);
        f.host.linkClosed();
        CHECK(f.host.count() == 0 && f.world.finished.empty() && f.world.events.empty() && f.world.speeds.size() == 1);
        f.host.settle();
        f.tick();
        CHECK(f.world.finished.empty() && f.world.events.empty());
        // The host is usable for the next link.
        CHECK(f.start(1, "script.onTick(t => script.emit(t));") == Ok);
        f.tick();
        CHECK(f.world.events.size() == 1);
    }
}

void testNames()
{
    CHECK(validName("nasa") && validName("a b~!") && validName(std::string(64, 'x')));
    CHECK(!validName("") && !validName(std::string(65, 'x')) && !validName("a\tb") && !validName("\xC3\xA5") &&
          !validName(std::string("a\0b", 3)));
    CHECK(threadCpuMicros() > 0);
}

// Many starts and ends: runtimes are freed (run under valgrind or ASan to see leaks).
void testChurn()
{
    Fixture f;
    f.world.addPlayer(3, "p", 1, true);
    for (uint32_t i = 1; i <= 200; ++i)
    {
        CHECK(f.start(i, "let n = 0; script.onTick(() => { game.player(3); if (++n === 2) script.finish(n); });") == Ok);
        f.tick(2);
    }
    CHECK(f.host.count() == 0 && f.world.finished.size() == 200);
}

// Bytes in use on the malloc heap. mallinfo2 is glibc 2.33; older ones (the CentOS 7 build) have mallinfo, whose
// int counters are enough for this test.
size_t heapInUse()
{
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
    return mallinfo2().uordblks;
#else
    return static_cast<size_t>(mallinfo().uordblks);
#endif
}

// What the host API costs (printed, not checked): CPU time per call from inside one script.
void printCosts()
{
    {
        Fixture f;
        const size_t before = heapInUse();
        for (uint32_t i = 1; i <= 8; ++i)
            f.start(i, "let n = 0; script.onTick(() => { n++; }); script.onStop(() => {});");
        f.tick(3);
        std::printf("  memory: %.0f KiB per idle script (8 running)\n", (heapInUse() - before) / 8.0 / 1024.0);
    }
    {
        // StartScript as a whole: the runtime, both contexts, compiling and the top-level code; then the end.
        Fixture f;
        const std::string source = "const { id, ceiling } = script.args;\nlet pushes = 0;\n"
                                   "script.onTick(() => {\n  const p = game.player(id);\n  if (!p || !p.position) return;\n"
                                   "  const over = p.position[1] - ceiling;\n  if (over <= 0) return;\n"
                                   "  game.setVelocity(id, [p.velocity[0], -over * 2, p.velocity[2]]); pushes++;\n});\n"
                                   "script.onStop(() => script.emit({ pushes }));\n";
        uint64_t starting = 0, stopping = 0;
        for (uint32_t i = 1; i <= 100; ++i)
        {
            const uint64_t t0 = threadCpuMicros();
            CHECK(f.host.start(i, 1, "ceiling", "{\"id\":3,\"ceiling\":300}", source, f.error) == Ok);
            const uint64_t t1 = threadCpuMicros();
            f.host.stop(i, f.error);
            f.host.settle();
            starting += t1 - t0;
            stopping += threadCpuMicros() - t1;
        }
        std::printf("  cost of starting a ten-line script %.0f us, of stopping it %.0f us\n", starting / 100.0, stopping / 100.0);
    }
    const struct { const char* what; const char* body; } cases[] = {
        { "an empty loop", "" },
        { "game.player(3)", "game.player(3);" },
        { "game.players()", "game.players();" },
        { "game.setVelocity", "game.setVelocity(3, v);" },
        { "game.teleport", "game.teleport(3, v, v);" },
        { "game.tick", "game.tick;" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        Fixture f;
        for (int id = 0; id < 4; ++id)
            f.world.addPlayer(id, "player", 1, true);
        f.world.all[3].vehicle = "ahe_ah1z";
        const std::string source = std::string("const v = [1, 2, 3]; for (let i = 0; i < 2000; i++) { ") + cases[i].body +
                                   " } script.finish();";
        CHECK(f.host.start(1, 1, "cost", "", source, f.error) == Ok);
        f.host.settle();
        if (!f.world.finished.empty())
            std::printf("  cost of %-18s %.3f us per call\n", cases[i].what, f.world.finished[0].totalUs / 2000.0);
        f.world.teleports.clear();
        f.world.speeds.clear();
    }
}

} // namespace

int main()
{
    testNames();
    testLifeCycle();
    testFinish();
    testArgumentsAndMessages();
    testStartErrors();
    testErrors();
    testTimeBudget();
    testMemoryAndStack();
    testEventLimits();
    testLanguage();
    testHostApi();
    testObjectApi();
    testHandlersAndOrder();
    testLevelUnloadAndLinkClosed();
    testChurn();
    printCosts();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
