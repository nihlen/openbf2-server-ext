// The script host (Script.h): QuickJS-NG runtimes, the host API and the life cycle of
// docs/admin-channel.md "Scripts".

#include "Script.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" {
#include "quickjs.h"
}

namespace serverext {
namespace script {

PlayerView::PlayerView()
    : id(0), team(0), alive(false), bot(false), seated(false), seat(-1), rootId(0), hasRoot(false)
{
    for (int i = 0; i < 3; ++i)
        pos[i] = rot[i] = vel[i] = 0.0f;
}

ObjectView::ObjectView()
    : id(0)
{
    for (int i = 0; i < 3; ++i)
        pos[i] = rot[i] = vel[i] = 0.0f;
}

uint64_t threadCpuMicros()
{
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

bool validName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxNameBytes)
        return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (static_cast<unsigned char>(name[i]) < 0x20 || static_cast<unsigned char>(name[i]) > 0x7E)
            return false;
    return true;
}

namespace {

// Replaces every byte that is not part of a well-formed UTF-8 sequence with '?': a script's strings
// can hold lone surrogates, and WebAdmin's reader is strict.
void sanitizeUtf8(std::string& s)
{
    size_t i = 0;
    const size_t n = s.size();
    while (i < n)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t cp = 0, min = 0;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; min = 0x10000; }
        bool good = len != 0 && i + len <= n;
        for (size_t k = 1; good && k < len; ++k)
        {
            const unsigned char t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80)
                good = false;
            cp = (cp << 6) | (t & 0x3F);
        }
        if (good && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)))
            good = false;
        if (good)
            i += len;
        else
            s[i++] = '?';
    }
}

// Cuts s to at most max bytes without splitting a UTF-8 sequence.
void cutUtf8(std::string& s, size_t max)
{
    if (s.size() <= max)
        return;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    s.resize(n);
}

const char* reasonText(uint8_t reason)
{
    switch (reason)
    {
    case ReasonFinished: return "finished";
    case ReasonStopped: return "stopped";
    case ReasonLevelUnload: return "level unload";
    default: return "";
    }
}

} // namespace

// ---------------------------------------------------------------- one script

class Script
{
public:
    enum Kind { Tick = 0, Message = 1, Stop = 2, KindCount = 3 };

    Script(Host* host, World* world, uint32_t instance, const std::string& name)
        : m_host(host), m_world(world), m_instance(instance), m_name(name), m_rt(0), m_ctx(0), m_ending(false),
          m_reason(0), m_finish(false), m_stopping(false), m_overBudget(false), m_callStart(0), m_deadline(0),
          m_budget(0), m_calls(0), m_totalUs(0), m_maxUs(0), m_windowSum(0), m_windowPos(0), m_events(0)
    {
        for (int i = 0; i < kWindowTicks; ++i)
            m_window[i] = 0;
    }

    ~Script()
    {
        if (m_ctx)
        {
            for (int k = 0; k < KindCount; ++k)
                for (size_t i = 0; i < m_handlers[k].size(); ++i)
                    JS_FreeValue(m_ctx, m_handlers[k][i]);
            JS_FreeContext(m_ctx);
        }
        if (m_rt)
            JS_FreeRuntime(m_rt);
    }

    uint32_t instance() const { return m_instance; }
    const std::string& name() const { return m_name; }
    bool ending() const { return m_ending; }
    uint8_t reason() const { return m_reason; }
    const std::string& endText() const { return m_endText; }
    uint32_t calls() const { return clamp32(m_calls); }
    uint32_t totalUs() const { return clamp32(m_totalUs); }
    uint32_t maxUs() const { return clamp32(m_maxUs); }

    // Creates the runtime, compiles the source, sets the globals and runs the top-level code.
    // @return False with error set; the script is then to be deleted.
    bool start(const std::string& argsJson, const std::string& source, std::string& error)
    {
        m_rt = JS_NewRuntime();
        if (!m_rt)
        {
            error = "out of memory";
            return false;
        }
        JS_SetMemoryLimit(m_rt, kMemoryLimit);
        JS_SetMaxStackSize(m_rt, kStackLimit);
        JS_SetCanBlock(m_rt, false);
        JS_SetInterruptHandler(m_rt, &Script::onInterrupt, this);
        JS_SetHostPromiseRejectionTracker(m_rt, &Script::onRejection, this);
        JS_UpdateStackTop(m_rt);

        // The parser lives only in a context of its own, which is freed before the script runs: the
        // script's context has no eval and no Function constructor. The compiled code moves over as
        // bytecode, which is safe here because this process wrote it a moment ago.
        std::vector<uint8_t> code;
        if (!compile(source, code, error))
            return false;

        m_ctx = JS_NewContextRaw(m_rt);
        if (!m_ctx || JS_AddIntrinsicBaseObjects(m_ctx) || JS_AddIntrinsicJSON(m_ctx) || JS_AddIntrinsicMapSet(m_ctx) ||
            JS_AddIntrinsicTypedArrays(m_ctx) || JS_AddIntrinsicPromise(m_ctx))
        {
            error = "out of memory";
            return false;
        }
        JS_SetContextOpaque(m_ctx, this);

        JSValue args = JS_NULL;
        if (!argsJson.empty())
        {
            args = JS_ParseJSON(m_ctx, argsJson.c_str(), argsJson.size(), "<arguments>");
            if (JS_IsException(args))
            {
                error = "the arguments are not JSON: " + takeException();
                return false;
            }
        }
        if (!installGlobals(args))
        {
            error = "out of memory";
            return false;
        }

        JSValue fn = JS_ReadObject(m_ctx, code.empty() ? 0 : &code[0], code.size(), JS_READ_OBJ_BYTECODE);
        if (JS_IsException(fn))
        {
            error = takeException();
            return false;
        }
        begin(kTopLevelBudgetUs);
        JSValue r = JS_EvalFunction(m_ctx, fn);   // takes fn
        if (JS_IsException(r))
            failFromException();
        else
        {
            JS_FreeValue(m_ctx, r);
            runJobs();
        }
        finishCall(false);
        if (m_finish && !m_ending)
            end(ReasonFinished, m_finishResult);
        if (m_ending && m_reason != ReasonFinished)
        {
            error = m_endText;
            return false;
        }
        return true;
    }

    void beginTick()
    {
        m_windowPos = (m_windowPos + 1) % kWindowTicks;
        m_windowSum -= m_window[m_windowPos];
        m_window[m_windowPos] = 0;
        m_events = 0;
    }

    void tick(uint32_t tickNumber)
    {
        if (m_ending || m_handlers[Tick].empty())
            return;
        JSValue arg = JS_NewUint32(m_ctx, tickNumber);
        dispatch(Tick, arg);
    }

    // @return False with error set when json is not JSON.
    bool message(const std::string& json, std::string& error)
    {
        if (m_ending)
            return true;
        JS_UpdateStackTop(m_rt);
        JSValue arg = JS_ParseJSON(m_ctx, json.c_str(), json.size(), "<message>");
        if (JS_IsException(arg))
        {
            error = "the message is not JSON: " + takeException();
            return false;
        }
        dispatch(Message, arg);
        JS_FreeValue(m_ctx, arg);
        return true;
    }

    // Marks the script to end; the first reason wins.
    void end(uint8_t reason, const std::string& text)
    {
        if (m_ending)
            return;
        m_ending = true;
        m_reason = reason;
        m_endText = text;
        sanitizeUtf8(m_endText);
        if (reason != ReasonFinished)
            cutUtf8(m_endText, kMaxPayloadBytes);
    }

    // The stop handlers, with the reason as text. Their errors are logged and ignored.
    void runStopHandlers(const char* reason)
    {
        if (m_handlers[Stop].empty())
            return;
        m_stopping = true;
        begin(kCallBudgetUs);
        JSValue arg = JS_NewString(m_ctx, reason);
        for (size_t i = 0; i < m_handlers[Stop].size() && !m_overBudget; ++i)
        {
            JSValue fn = JS_DupValue(m_ctx, m_handlers[Stop][i]);
            JSValue r = JS_Call(m_ctx, fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(m_ctx, fn);
            if (JS_IsException(r))
                m_world->log(m_instance, m_name, "stop handler: " + takeException());
            else
                JS_FreeValue(m_ctx, r);
        }
        JS_FreeValue(m_ctx, arg);
        if (!m_overBudget)
            runJobs();
        finishCall(false);
        m_stopping = false;
    }

private:
    static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }
    static Script* self(JSContext* ctx) { return static_cast<Script*>(JS_GetContextOpaque(ctx)); }

    // ---- compiling

    bool compile(const std::string& source, std::vector<uint8_t>& code, std::string& error)
    {
        JSContext* c = JS_NewContextRaw(m_rt);
        if (!c || JS_AddIntrinsicBaseObjects(c) || JS_AddIntrinsicEval(c))
        {
            if (c)
                JS_FreeContext(c);
            error = "out of memory";
            return false;
        }
        bool ok = false;
        JSValue fn = JS_Eval(c, source.c_str(), source.size(), m_name.c_str(),
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(fn))
            error = describeException(c);
        else
        {
            size_t size = 0;
            uint8_t* buf = JS_WriteObject(c, &size, fn, JS_WRITE_OBJ_BYTECODE);
            JS_FreeValue(c, fn);
            if (!buf)
                error = describeException(c);
            else
            {
                code.assign(buf, buf + size);
                js_free(c, buf);
                ok = true;
            }
        }
        JS_FreeContext(c);
        return ok;
    }

    // ---- entering and leaving a script

    void begin(uint64_t budgetUs)
    {
        JS_UpdateStackTop(m_rt);
        m_overBudget = false;
        m_budget = budgetUs;
        m_callStart = m_world->cpuMicros();
        m_deadline = m_callStart + budgetUs;
    }

    // Books the call's CPU time and ends the script when it is over a budget.
    void finishCall(bool inWindow)
    {
        const uint64_t now = m_world->cpuMicros();
        const uint64_t us = now > m_callStart ? now - m_callStart : 0;
        ++m_calls;
        m_totalUs += us;
        if (us > m_maxUs)
            m_maxUs = us;
        ++m_host->m_stats.calls;
        m_host->m_stats.totalUs += us;
        m_host->m_tickUs += us;
        if (inWindow)
        {
            m_window[m_windowPos] += us;
            m_windowSum += us;
        }
        if (m_stopping)
        {
            if (m_overBudget || us > m_budget)
                m_world->log(m_instance, m_name, "stop handler: over the time budget");
            return;
        }
        char buf[160];
        if (m_overBudget || us > m_budget)
        {
            std::snprintf(buf, sizeof(buf), "over the time budget: a call took %.1f ms (limit %.0f ms)", us / 1000.0,
                          m_budget / 1000.0);
            overBudget(buf);
        }
        else if (inWindow && !m_ending && m_windowSum > kWindowBudgetUs)
        {
            std::snprintf(buf, sizeof(buf), "over the time budget: %.1f ms in %d ticks (limit %.0f ms)",
                          m_windowSum / 1000.0, kWindowTicks, kWindowBudgetUs / 1000.0);
            overBudget(buf);
        }
    }

    // Reason 4 replaces an error the interrupted call left behind (the interruption itself).
    void overBudget(const std::string& text)
    {
        if (m_ending && m_reason != ReasonError && m_reason != ReasonFinished)
            return;
        std::string full = text;
        if (m_ending && m_reason == ReasonError && !m_interruptStack.empty())
            full += "\n" + m_interruptStack;
        m_ending = false;
        m_finish = false;
        end(ReasonBudget, full);
    }

    static int onInterrupt(JSContext*, void* opaque)
    {
        Script* s = static_cast<Script*>(opaque);
        if (s->m_world->cpuMicros() <= s->m_deadline)
            return 0;
        s->m_overBudget = true;
        return 1;
    }

    static void onRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool isHandled, void* opaque)
    {
        Script* s = static_cast<Script*>(opaque);
        void* key = JS_VALUE_GET_PTR(promise);
        if (isHandled)
        {
            for (size_t i = 0; i < s->m_rejected.size(); ++i)
                if (s->m_rejected[i].first == key)
                {
                    s->m_rejected.erase(s->m_rejected.begin() + i);
                    break;
                }
            return;
        }
        if (s->m_rejected.size() < 8)
            s->m_rejected.push_back(std::make_pair(key, "unhandled promise rejection: " + describeValue(ctx, reason)));
    }

    // The promise jobs a handler left, then the rejections nobody handled.
    void runJobs()
    {
        for (int n = 0; !m_ending && !m_overBudget; ++n)
        {
            JSContext* c = 0;
            const int r = JS_ExecutePendingJob(m_rt, &c);
            if (r == 0)
                break;
            if (r < 0)
            {
                if (!m_stopping)
                    failFromException();
                else
                    m_world->log(m_instance, m_name, "stop handler: " + takeException());
                break;
            }
            if ((n & 63) == 63 && m_world->cpuMicros() > m_deadline)
                m_overBudget = true;
        }
        if (!m_rejected.empty())
        {
            if (!m_stopping)
                end(ReasonError, m_rejected[0].second);
            else
                m_world->log(m_instance, m_name, "stop handler: " + m_rejected[0].second);
            m_rejected.clear();
        }
    }

    // Calls every handler of a kind with one argument (not consumed), then the promise jobs.
    void dispatch(Kind kind, JSValueConst arg)
    {
        begin(kCallBudgetUs);
        for (size_t i = 0; i < m_handlers[kind].size() && !m_ending && !m_finish; ++i)
        {
            JSValue fn = JS_DupValue(m_ctx, m_handlers[kind][i]);
            JSValue r = JS_Call(m_ctx, fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(m_ctx, fn);
            if (JS_IsException(r))
                failFromException();
            else
                JS_FreeValue(m_ctx, r);
        }
        if (!m_ending && !m_finish)
            runJobs();
        finishCall(true);
        if (m_finish && !m_ending)
            end(ReasonFinished, m_finishResult);
    }

    // ---- errors as text

    static std::string toStdString(JSContext* ctx, JSValueConst v)
    {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (!s)
        {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return std::string();
        }
        std::string out(s, len);
        JS_FreeCString(ctx, s);
        return out;
    }

    // A string property of an object, or empty.
    static std::string stringProperty(JSContext* ctx, JSValueConst v, const char* name)
    {
        std::string out;
        JSValue p = JS_GetPropertyStr(ctx, v, name);
        if (JS_IsString(p))
            out = toStdString(ctx, p);
        if (JS_IsException(p))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_FreeValue(ctx, p);
        return out;
    }

    // "Name: message" and the stack for an Error, else the value as a string.
    static std::string describeValue(JSContext* ctx, JSValueConst v)
    {
        std::string out = toStdString(ctx, v);
        if (out.empty())
            out = "error";
        if (JS_IsError(v))
        {
            // The parser's errors convert to their message alone; every error is to carry its name.
            const std::string name = stringProperty(ctx, v, "name");
            if (!name.empty() && out.compare(0, name.size(), name) != 0)
                out = name + ": " + out;
            JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
            if (JS_IsString(stack))
            {
                std::string s = toStdString(ctx, stack);
                while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == ' '))
                    s.resize(s.size() - 1);
                if (!s.empty())
                    out += "\n" + s;
            }
            if (JS_IsException(stack))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_FreeValue(ctx, stack);
        }
        return out;
    }

    static std::string describeException(JSContext* ctx)
    {
        JSValue e = JS_GetException(ctx);
        std::string out = describeValue(ctx, e);
        JS_FreeValue(ctx, e);
        return out;
    }

    std::string takeException()
    {
        // QuickJS throws "out of memory" as an InternalError, but when that error object cannot be made either,
        // the exception is left null. Near the limit a null exception is taken for that.
        JSValue e = JS_GetException(m_ctx);
        const bool null = JS_IsNull(e) || JS_IsUndefined(e) || JS_IsUninitialized(e);
        JS_Throw(m_ctx, e);
        if (null)
        {
            JSMemoryUsage use;
            JS_ComputeMemoryUsage(m_rt, &use);
            if (use.malloc_size > static_cast<int64_t>(kMemoryLimit) - 64 * 1024)
            {
                JS_FreeValue(m_ctx, JS_GetException(m_ctx));
                return "InternalError: out of memory";
            }
        }
        return describeException(m_ctx);
    }

    void failFromException()
    {
        const std::string text = takeException();
        if (m_overBudget)
            m_interruptStack = text;
        if (m_stopping)
            return;
        if (m_finish)
            return;   // finish() was called before the error: the script ends as finished
        end(ReasonError, text);
    }

    // ---- arguments

    static bool argId(JSContext* ctx, JSValueConst v, const char* what, int& id)
    {
        double d = 0.0;
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || !(d == std::floor(d)) || !std::isfinite(d))
        {
            JS_ThrowTypeError(ctx, "%s must be an integer", what);
            return false;
        }
        id = d < -1.0 || d > 100000.0 ? -2 : static_cast<int>(d);
        return true;
    }

    // Reads [a, b, c] (an array or anything indexable) into out. bound: each |value| must not exceed it.
    static bool argVec3(JSContext* ctx, JSValueConst v, const char* what, float bound, float out[3])
    {
        if (!JS_IsObject(v))
        {
            JS_ThrowTypeError(ctx, "%s must be an array of three numbers", what);
            return false;
        }
        for (uint32_t i = 0; i < 3; ++i)
        {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            if (JS_IsException(e))
                return false;
            double d = 0.0;
            const bool number = JS_IsNumber(e) && JS_ToFloat64(ctx, &d, e) == 0;
            JS_FreeValue(ctx, e);
            if (!number)
            {
                JS_ThrowTypeError(ctx, "%s must be an array of three numbers", what);
                return false;
            }
            if (!std::isfinite(d) || std::fabs(d) > bound)
            {
                JS_ThrowRangeError(ctx, "%s[%u] is not finite or out of range", what, i);
                return false;
            }
            out[i] = static_cast<float>(d);
        }
        return true;
    }

    static JSValue vec3(JSContext* ctx, const float v[3])
    {
        JSValue values[3];
        for (int i = 0; i < 3; ++i)
            values[i] = JS_NewFloat64(ctx, static_cast<double>(v[i]));
        return JS_NewArrayFrom(ctx, 3, values);
    }

    static bool isNullish(JSValueConst v) { return JS_IsNull(v) || JS_IsUndefined(v); }

    // ---- the script object

    static JSValue jsOn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int kind)
    {
        Script* s = self(ctx);
        if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "the handler must be a function");
        if (!s->m_stopping && s->m_handlers[kind].size() < 64)
            s->m_handlers[kind].push_back(JS_DupValue(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    // JSON.stringify(v) as UTF-8. @return False with an exception pending; undefined is "not serialisable".
    static bool stringify(JSContext* ctx, JSValueConst v, const char* what, std::string& out)
    {
        JSValue text = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(text))
            return false;
        if (!JS_IsString(text))
        {
            JS_FreeValue(ctx, text);
            JS_ThrowTypeError(ctx, "%s must be JSON-serialisable", what);
            return false;
        }
        size_t len = 0;
        const char* p = JS_ToCStringLen(ctx, &len, text);
        JS_FreeValue(ctx, text);
        if (!p)
            return false;
        out.assign(p, len);
        JS_FreeCString(ctx, p);
        sanitizeUtf8(out);
        return true;
    }

    static JSValue jsFinish(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        std::string result;
        if (argc >= 1 && !JS_IsUndefined(argv[0]))
        {
            if (!stringify(ctx, argv[0], "the result", result))
                return JS_EXCEPTION;
            if (result.size() > kMaxResultBytes)
                return JS_ThrowRangeError(ctx, "the result is longer than %u bytes of JSON",
                                          static_cast<unsigned>(kMaxResultBytes));
        }
        if (!s->m_stopping && !s->m_finish && !s->m_ending)
        {
            s->m_finish = true;
            s->m_finishResult = result;
        }
        return JS_UNDEFINED;
    }

    void queueEvent(uint8_t kind, const std::string& payload)
    {
        Host::Outbound o;
        o.finished = false;
        o.instance = m_instance;
        o.code = kind;
        o.text = payload;
        o.calls = o.totalUs = o.maxUs = 0;
        m_host->m_outbox.push_back(o);
        ++m_events;
    }

    static JSValue jsEmit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        std::string payload;
        if (!stringify(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the data", payload))
            return JS_EXCEPTION;
        if (payload.size() > kMaxPayloadBytes)
            return JS_ThrowRangeError(ctx, "the data is longer than %u bytes of JSON",
                                      static_cast<unsigned>(kMaxPayloadBytes));
        if (s->m_events >= kMaxEventsPerTick)
            return JS_ThrowRangeError(ctx, "more than %d script events in one tick", kMaxEventsPerTick);
        s->queueEvent(EventData, payload);
        return JS_UNDEFINED;
    }

    static JSValue jsLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        std::string line;
        for (int i = 0; i < argc; ++i)
        {
            size_t len = 0;
            const char* p = JS_ToCStringLen(ctx, &len, argv[i]);
            if (!p)
                return JS_EXCEPTION;
            if (i)
                line += ' ';
            if (line.size() < kMaxPayloadBytes + 8)
                line.append(p, len);
            JS_FreeCString(ctx, p);
        }
        if (s->m_events >= kMaxEventsPerTick)
        {
            ++s->m_host->m_stats.logsDropped;
            return JS_UNDEFINED;
        }
        sanitizeUtf8(line);
        cutUtf8(line, kMaxPayloadBytes);
        s->m_world->log(s->m_instance, s->m_name, line);
        s->queueEvent(EventLog, line);
        return JS_UNDEFINED;
    }

    // ---- the game object

    static JSValue jsTick(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return JS_NewUint32(ctx, self(ctx)->m_world->tick());
    }

    static JSValue jsTime(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return JS_NewFloat64(ctx, self(ctx)->m_world->time());
    }

    static JSValue jsMap(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        std::string map = self(ctx)->m_world->map();
        sanitizeUtf8(map);
        return JS_NewStringLen(ctx, map.data(), map.size());
    }

    static JSValue jsPlayers(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        Script* s = self(ctx);
        s->m_ids.clear();
        s->m_world->players(s->m_ids);
        JSValue a = JS_NewArray(ctx);
        if (JS_IsException(a))
            return a;
        for (size_t i = 0; i < s->m_ids.size(); ++i)
            if (JS_SetPropertyUint32(ctx, a, static_cast<uint32_t>(i), JS_NewInt32(ctx, s->m_ids[i])) < 0)
            {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
        return a;
    }

    static bool set(JSContext* ctx, JSValueConst o, const char* name, JSValue v)
    {
        if (JS_IsException(v))
            return false;
        return JS_DefinePropertyValueStr(ctx, o, name, v, JS_PROP_C_W_E) >= 0;
    }

    static JSValue jsPlayer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        int id = 0;
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the player id", id))
            return JS_EXCEPTION;
        PlayerView p;
        if (id < 0 || id > 255 || !s->m_world->player(id, p))
            return JS_NULL;
        sanitizeUtf8(p.name);
        sanitizeUtf8(p.vehicle);
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o))
            return o;
        const bool ok =
            set(ctx, o, "id", JS_NewInt32(ctx, p.id)) &&
            set(ctx, o, "name", JS_NewStringLen(ctx, p.name.data(), p.name.size())) &&
            set(ctx, o, "team", JS_NewInt32(ctx, p.team)) &&
            set(ctx, o, "alive", JS_NewBool(ctx, p.alive)) &&
            set(ctx, o, "bot", JS_NewBool(ctx, p.bot)) &&
            set(ctx, o, "seated", JS_NewBool(ctx, p.seated)) &&
            set(ctx, o, "seat", JS_NewInt32(ctx, p.seat)) &&
            set(ctx, o, "rootId", JS_NewUint32(ctx, p.rootId)) &&
            set(ctx, o, "vehicle", JS_NewStringLen(ctx, p.vehicle.data(), p.vehicle.size())) &&
            set(ctx, o, "position", p.hasRoot ? vec3(ctx, p.pos) : JS_NULL) &&
            set(ctx, o, "rotation", p.hasRoot ? vec3(ctx, p.rot) : JS_NULL) &&
            set(ctx, o, "velocity", p.hasRoot ? vec3(ctx, p.vel) : JS_NULL);
        if (!ok)
        {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }

    static JSValue jsTeleport(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        int id = 0;
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the player id", id))
            return JS_EXCEPTION;
        const bool hasPos = argc >= 2 && !isNullish(argv[1]);
        const bool hasRot = argc >= 3 && !isNullish(argv[2]);
        if (!hasPos && !hasRot)
            return JS_ThrowTypeError(ctx, "a position or a rotation is needed");
        float pos[3], rot[3];
        if (hasPos)
        {
            if (!argVec3(ctx, argv[1], "the position", 10000.0f, pos))
                return JS_EXCEPTION;
            // The Teleport command's world bounds (docs/protocol.md, Teleport).
            if (std::fabs(pos[0]) > 8192.0f || std::fabs(pos[2]) > 8192.0f || pos[1] < -1000.0f)
                return JS_ThrowRangeError(ctx, "the position is outside the world");
        }
        if (hasRot && !argVec3(ctx, argv[2], "the rotation", 1.0e6f, rot))
            return JS_EXCEPTION;
        if (id < 0 || id > 255)
            return JS_FALSE;
        return JS_NewBool(ctx, s->m_world->teleport(id, hasPos ? pos : 0, hasRot ? rot : 0));
    }

    static JSValue jsSetSpeed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int angular)
    {
        Script* s = self(ctx);
        int id = 0;
        float v[3];
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the player id", id) ||
            !argVec3(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED, angular ? "the angular velocity" : "the velocity",
                     angular ? kMaxAngularSpeed : kMaxSpeed, v))
            return JS_EXCEPTION;
        if (id < 0 || id > 255)
            return JS_FALSE;
        return JS_NewBool(ctx, angular ? s->m_world->setAngularVelocity(id, v) : s->m_world->setVelocity(id, v));
    }

    // ---- API 2: objects by networkable id

    static bool validNetId(int id) { return id >= 1 && id <= 0xFFFF; }

    static JSValue jsObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        int id = 0;
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the networkable id", id))
            return JS_EXCEPTION;
        ObjectView v;
        if (!validNetId(id) || !s->m_world->object(static_cast<uint32_t>(id), v))
            return JS_NULL;
        sanitizeUtf8(v.templ);
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o))
            return o;
        const bool ok =
            set(ctx, o, "id", JS_NewUint32(ctx, v.id)) &&
            set(ctx, o, "template", JS_NewStringLen(ctx, v.templ.data(), v.templ.size())) &&
            set(ctx, o, "position", vec3(ctx, v.pos)) &&
            set(ctx, o, "rotation", vec3(ctx, v.rot)) &&
            set(ctx, o, "velocity", vec3(ctx, v.vel));
        if (!ok)
        {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }

    static JSValue jsTeleportObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        int id = 0;
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the networkable id", id))
            return JS_EXCEPTION;
        const bool hasPos = argc >= 2 && !isNullish(argv[1]);
        const bool hasRot = argc >= 3 && !isNullish(argv[2]);
        if (!hasPos && !hasRot)
            return JS_ThrowTypeError(ctx, "a position or a rotation is needed");
        float pos[3], rot[3];
        if (hasPos)
        {
            if (!argVec3(ctx, argv[1], "the position", 10000.0f, pos))
                return JS_EXCEPTION;
            if (std::fabs(pos[0]) > 8192.0f || std::fabs(pos[2]) > 8192.0f || pos[1] < -1000.0f)
                return JS_ThrowRangeError(ctx, "the position is outside the world");
        }
        if (hasRot && !argVec3(ctx, argv[2], "the rotation", 1.0e6f, rot))
            return JS_EXCEPTION;
        if (!validNetId(id))
            return JS_FALSE;
        return JS_NewBool(ctx, s->m_world->teleportObject(static_cast<uint32_t>(id), hasPos ? pos : 0, hasRot ? rot : 0));
    }

    static JSValue jsSetObjectSpeed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int angular)
    {
        Script* s = self(ctx);
        int id = 0;
        float v[3];
        if (!argId(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, "the networkable id", id) ||
            !argVec3(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED, angular ? "the angular velocity" : "the velocity",
                     angular ? kMaxAngularSpeed : kMaxSpeed, v))
            return JS_EXCEPTION;
        if (!validNetId(id))
            return JS_FALSE;
        const uint32_t n = static_cast<uint32_t>(id);
        return JS_NewBool(ctx, angular ? s->m_world->setObjectAngularVelocity(n, v) : s->m_world->setObjectVelocity(n, v));
    }

    static JSValue jsSay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Script* s = self(ctx);
        if (argc < 1 || !JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "the text must be a string");
        int player = -1;
        if (argc >= 2 && !isNullish(argv[1]))
        {
            if (!argId(ctx, argv[1], "the player id", player))
                return JS_EXCEPTION;
            if (player < 0 || player > 255)
                return JS_UNDEFINED;
        }
        std::string text = toStdString(ctx, argv[0]);
        sanitizeUtf8(text);
        std::string clean;
        for (size_t i = 0; i < text.size(); ++i)
            if (static_cast<unsigned char>(text[i]) >= 0x20 && text[i] != 0x7F)
                clean += text[i];
        cutUtf8(clean, kMaxSayBytes);
        if (!clean.empty())
            s->m_world->say(clean, player);
        return JS_UNDEFINED;
    }

    bool defineGetter(JSValueConst obj, const char* name, JSCFunction* fn)
    {
        JSAtom atom = JS_NewAtom(m_ctx, name);
        JSValue getter = JS_NewCFunction(m_ctx, fn, name, 0);
        const int r = JS_DefinePropertyGetSet(m_ctx, obj, atom, getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
        JS_FreeAtom(m_ctx, atom);
        return r >= 0;
    }

    bool defineFn(JSValueConst obj, const char* name, JSCFunction* fn, int length)
    {
        return set(m_ctx, obj, name, JS_NewCFunction(m_ctx, fn, name, length));
    }

    bool defineMagic(JSValueConst obj, const char* name, JSCFunctionMagic* fn, int length, int magic)
    {
        return set(m_ctx, obj, name, JS_NewCFunctionMagic(m_ctx, fn, name, length, JS_CFUNC_generic_magic, magic));
    }

    // The frozen script and game globals. Takes args.
    bool installGlobals(JSValue args)
    {
        JSValue script = JS_NewObject(m_ctx);
        JSValue game = JS_NewObject(m_ctx);
        bool ok = !JS_IsException(script) && !JS_IsException(game);
        ok = ok && set(m_ctx, script, "api", JS_NewInt32(m_ctx, kApiVersion));
        ok = ok && set(m_ctx, script, "name", JS_NewStringLen(m_ctx, m_name.data(), m_name.size()));
        if (ok)
            ok = set(m_ctx, script, "args", args);
        else
            JS_FreeValue(m_ctx, args);
        ok = ok && defineMagic(script, "onTick", &Script::jsOn, 1, Tick);
        ok = ok && defineMagic(script, "onMessage", &Script::jsOn, 1, Message);
        ok = ok && defineMagic(script, "onStop", &Script::jsOn, 1, Stop);
        ok = ok && defineFn(script, "finish", &Script::jsFinish, 1);
        ok = ok && defineFn(script, "emit", &Script::jsEmit, 1);
        ok = ok && defineFn(script, "log", &Script::jsLog, 0);

        ok = ok && defineGetter(game, "tick", &Script::jsTick);
        ok = ok && set(m_ctx, game, "tickRate", JS_NewInt32(m_ctx, 30));
        ok = ok && defineGetter(game, "time", &Script::jsTime);
        ok = ok && defineGetter(game, "map", &Script::jsMap);
        ok = ok && defineFn(game, "players", &Script::jsPlayers, 0);
        ok = ok && defineFn(game, "player", &Script::jsPlayer, 1);
        ok = ok && defineFn(game, "teleport", &Script::jsTeleport, 3);
        ok = ok && defineMagic(game, "setVelocity", &Script::jsSetSpeed, 2, 0);
        ok = ok && defineMagic(game, "setAngularVelocity", &Script::jsSetSpeed, 2, 1);
        ok = ok && defineFn(game, "object", &Script::jsObject, 1);
        ok = ok && defineFn(game, "teleportObject", &Script::jsTeleportObject, 3);
        ok = ok && defineMagic(game, "setObjectVelocity", &Script::jsSetObjectSpeed, 2, 0);
        ok = ok && defineMagic(game, "setObjectAngularVelocity", &Script::jsSetObjectSpeed, 2, 1);
        ok = ok && defineFn(game, "say", &Script::jsSay, 2);
        ok = ok && JS_FreezeObject(m_ctx, script) >= 0 && JS_FreezeObject(m_ctx, game) >= 0;

        JSValue global = JS_GetGlobalObject(m_ctx);
        if (ok)
        {
            ok = JS_DefinePropertyValueStr(m_ctx, global, "script", script, JS_PROP_ENUMERABLE) >= 0;
            script = JS_UNDEFINED;
        }
        if (ok)
        {
            ok = JS_DefinePropertyValueStr(m_ctx, global, "game", game, JS_PROP_ENUMERABLE) >= 0;
            game = JS_UNDEFINED;
        }
        JS_FreeValue(m_ctx, global);
        JS_FreeValue(m_ctx, script);
        JS_FreeValue(m_ctx, game);
        if (!ok)
            JS_FreeValue(m_ctx, JS_GetException(m_ctx));
        return ok;
    }

    Host* m_host;
    World* m_world;
    uint32_t m_instance;
    std::string m_name;
    JSRuntime* m_rt;
    JSContext* m_ctx;
    std::vector<JSValue> m_handlers[KindCount];
    std::vector<std::pair<void*, std::string> > m_rejected;   // rejected promises nobody handles yet
    std::vector<int> m_ids;

    bool m_ending;              // due to end at the next settle
    uint8_t m_reason;
    std::string m_endText;      // the result (reason 1) or the error text (3, 4)
    bool m_finish;              // finish() was called in the running call
    std::string m_finishResult;
    bool m_stopping;            // the stop handlers run
    std::string m_interruptStack;

    bool m_overBudget;          // the interrupt handler ended the running call
    uint64_t m_callStart, m_deadline, m_budget;
    uint64_t m_calls, m_totalUs, m_maxUs;
    uint64_t m_window[kWindowTicks];
    uint64_t m_windowSum;
    int m_windowPos;
    int m_events;               // ScriptEvents queued in this tick
};

// ---------------------------------------------------------------- the host

Host::Host(World* world) : m_world(world), m_inTick(false), m_tickUs(0) {}

Host::~Host()
{
    for (size_t i = 0; i < m_scripts.size(); ++i)
        delete m_scripts[i];
}

Script* Host::find(uint32_t instance) const
{
    for (size_t i = 0; i < m_scripts.size(); ++i)
        if (m_scripts[i]->instance() == instance)
            return m_scripts[i];
    return 0;
}

uint16_t Host::start(uint32_t instance, uint8_t apiVersion, const std::string& name, const std::string& argsJson,
                     const std::string& source, std::string& error)
{
    if (apiVersion == 0 || instance == 0)
    {
        error = apiVersion == 0 ? "the script API version must be 1 or more" : "the instance must not be 0";
        return InvalidValue;
    }
    if (apiVersion > kApiVersion)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "script API %u is not available (this module has %u)",
                      static_cast<unsigned>(apiVersion), static_cast<unsigned>(kApiVersion));
        error = buf;
        return Unavailable;
    }
    if (!validName(name))
    {
        error = "the name must be 1..64 printable ASCII bytes";
        return InvalidValue;
    }
    if (source.empty() || source.size() > kMaxSourceBytes)
    {
        error = "the source must be 1..60000 bytes";
        return InvalidValue;
    }
    if (find(instance))
    {
        error = "the instance is in use";
        return Refused;
    }
    if (m_scripts.size() >= kMaxScripts)
    {
        error = "8 scripts run already";
        return Refused;
    }
    const size_t queued = m_outbox.size();
    Script* s = new Script(this, m_world, instance, name);
    if (!s->start(argsJson, source, error))
    {
        delete s;
        m_outbox.resize(queued);   // what a failed start emitted is not sent
        if (error.empty())
            error = "the script failed";
        return InvalidValue;
    }
    m_scripts.push_back(s);
    return Ok;
}

uint16_t Host::stop(uint32_t instance, std::string& error)
{
    Script* s = find(instance);
    if (!s)
    {
        error = "no such script";
        return Unavailable;
    }
    s->end(ReasonStopped, std::string());
    return Ok;
}

uint16_t Host::message(uint32_t instance, const std::string& json, std::string& error)
{
    Script* s = find(instance);
    if (!s)
    {
        error = "no such script";
        return Unavailable;
    }
    if (json.size() > kMaxPayloadBytes)
    {
        error = "the message is longer than 8192 bytes";
        return InvalidValue;
    }
    return s->message(json, error) ? Ok : InvalidValue;
}

void Host::endDue(bool report)
{
    for (size_t i = 0; i < m_scripts.size();)
    {
        Script* s = m_scripts[i];
        if (!s->ending())
        {
            ++i;
            continue;
        }
        const uint8_t reason = s->reason();
        if (reason == ReasonFinished || reason == ReasonStopped || reason == ReasonLevelUnload)
            s->runStopHandlers(reasonText(reason));
        if (report)
        {
            Outbound o;
            o.finished = true;
            o.instance = s->instance();
            o.name = s->name();
            o.code = reason;
            o.text = reason == ReasonFinished || reason == ReasonError || reason == ReasonBudget ? s->endText() : std::string();
            o.calls = s->calls();
            o.totalUs = s->totalUs();
            o.maxUs = s->maxUs();
            m_outbox.push_back(o);
        }
        delete s;
        m_scripts.erase(m_scripts.begin() + i);
    }
}

void Host::flush()
{
    // Swapped out first: nothing the world does with an event may grow the list being walked.
    std::vector<Outbound> out;
    out.swap(m_outbox);
    for (size_t i = 0; i < out.size(); ++i)
    {
        const Outbound& o = out[i];
        if (o.finished)
            m_world->scriptFinished(o.instance, o.name, o.code, o.text, o.calls, o.totalUs, o.maxUs);
        else
            m_world->scriptEvent(o.instance, o.code, o.text);
    }
}

void Host::settle()
{
    endDue(true);
    flush();
}

void Host::tick()
{
    m_tickUs = 0;
    const uint32_t tick = m_world->tick();
    for (size_t i = 0; i < m_scripts.size(); ++i)
    {
        m_scripts[i]->beginTick();
        m_scripts[i]->tick(tick);
    }
    if (m_tickUs > m_stats.maxTickUs)
        m_stats.maxTickUs = m_tickUs;
    settle();
}

void Host::levelUnload()
{
    for (size_t i = 0; i < m_scripts.size(); ++i)
        m_scripts[i]->end(ReasonLevelUnload, std::string());
    settle();
}

void Host::linkClosed()
{
    for (size_t i = 0; i < m_scripts.size(); ++i)
    {
        Script* s = m_scripts[i];
        if (!s->ending() || s->reason() == ReasonFinished || s->reason() == ReasonStopped ||
            s->reason() == ReasonLevelUnload)
            s->runStopHandlers("link closed");
        delete s;
    }
    m_scripts.clear();
    m_outbox.clear();
}

void Host::list(std::vector<std::pair<uint32_t, std::string> >& out) const
{
    out.clear();
    for (size_t i = 0; i < m_scripts.size(); ++i)
        out.push_back(std::make_pair(m_scripts[i]->instance(), m_scripts[i]->name()));
}

Stats Host::takeStats()
{
    const Stats s = m_stats;
    m_stats = Stats();
    return s;
}

} // namespace script
} // namespace serverext
