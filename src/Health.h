// Server health (docs/admin-channel.md "Server health"): how regularly the server's ticks run, how much of the main
// thread's CPU a tick takes, and what the host does to the process. The figures go to the module's log once a minute
// and to the admin channel's ServerHealth event every 10 s.
//
// Engine-free. Sampler and the two parsers have no clock and no file access, so the unit tests drive them. Monitor
// adds the clocks, getrusage and the host thread: its tick() runs on the server's main thread and makes no file or
// socket call; /proc is read by a thread of its own.

#ifndef SERVEREXT_HEALTH_H
#define SERVEREXT_HEALTH_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace serverext {
namespace health {

const unsigned int kEventWindowTicks = 300;    // ServerHealth: one window per 10 s
const unsigned int kLogWindowTicks = 1800;     // the log line: one per minute
// The engine's frames are shorter than a tick (28.3 ms by default), so in normal running a tick follows the one before
// by one frame or by two (56.7 ms). The limits are in tick lengths, clear of both.
const uint32_t kLateIntervalUs = 66667;        // above two ticks: the simulation stood still
const uint32_t kShortIntervalUs = 16667;       // below half a tick: the tick ran in the frame of the one before

// Window flags.
const uint8_t kFlagStepped = 1;      // stepped time is on: the tick figures do not describe a real-time server
const uint8_t kFlagHostValid = 2;    // the host figures are valid
const uint8_t kFlagLevelLoad = 4;    // a level was unloaded during the window: its load is in the tick figures

// One window of ticks. The layout of ServerHealth after its sequence number and tick.
struct Window
{
    uint16_t ticks;                 // ticks in the window
    uint32_t wallUs;                // real time the window took
    uint8_t flags;
    uint32_t intervalP50Us;         // real time from one tick to the next
    uint32_t intervalP99Us;
    uint32_t intervalMaxUs;
    uint16_t lateTicks;             // intervals above kLateIntervalUs
    uint16_t shortTicks;            // intervals below kShortIntervalUs
    uint32_t cpuP50Us;              // the main thread's CPU time from one tick to the next
    uint32_t cpuP99Us;
    uint32_t cpuMaxUs;
    uint32_t handlerP99Us;          // the module's own tick handler
    uint32_t handlerMaxUs;
    uint32_t involuntarySwitches;   // the main thread was taken off the CPU while it wanted to run
    uint32_t majorFaults;           // the main thread waited for a page to be read from disk
    uint16_t cpus;                  // the host's CPUs
    uint16_t hostBusy;              // hundredths of a percent of all CPUs
    uint16_t hostSteal;
    uint16_t hostIoWait;
    uint32_t residentKb;            // the process's resident memory, KiB

    Window();
};

// Collects one figure set per window of ticks.
class Sampler
{
public:
    explicit Sampler(unsigned int windowTicks);

    // Once per tick, at the start of the tick handler. The first call only sets the starting point.
    // @param wallUs A steady clock, microseconds.
    // @param cpuUs The main thread's CPU time, microseconds.
    // @param handlerUs What the module's tick handler took on the tick before.
    // @return True when this tick completes a window; window() holds it until the next one completes.
    bool add(uint64_t wallUs, uint64_t cpuUs, uint32_t handlerUs);

    // Marks the window in progress with kFlagLevelLoad.
    void noteLevelLoad() { m_pendingFlags |= kFlagLevelLoad; }

    // The last completed window. Its switch, fault and host fields are 0: Monitor fills them.
    Window& window() { return m_window; }
    const Window& window() const { return m_window; }

private:
    unsigned int m_windowTicks;
    bool m_started;
    uint64_t m_lastWallUs;
    uint64_t m_lastCpuUs;
    uint64_t m_windowStartUs;
    uint8_t m_pendingFlags;
    std::vector<uint32_t> m_intervals;
    std::vector<uint32_t> m_cpu;
    std::vector<uint32_t> m_handler;
    Window m_window;
};

// The q-quantile (0 < q <= 1) of values, which it reorders: the smallest value with at least q of the values at or
// below it. 0 for no values.
uint32_t quantile(std::vector<uint32_t>& values, double q);

// The first line of /proc/stat in clock ticks, and the number of "cpuN" lines.
struct CpuTimes
{
    uint64_t busy;      // user, nice, system, irq and softirq
    uint64_t steal;
    uint64_t ioWait;
    uint64_t total;     // busy, idle, iowait and steal
    uint16_t cpus;

    CpuTimes() : busy(0), steal(0), ioWait(0), total(0), cpus(0) {}
};

// Parses the text of /proc/stat.
// @return False when the "cpu" line is missing or has fewer than the eight fields up to steal.
bool parseProcStat(const char* text, size_t size, CpuTimes& out);

// Parses the text of /proc/self/statm.
// @return False when the second field, the resident pages, is missing or not a number.
bool parseStatm(const char* text, size_t size, uint64_t& residentPages);

// Hundredths of a percent that part is of whole, 0..10000. 0 when whole is 0.
uint16_t share(uint64_t part, uint64_t whole);

// What the host thread reads once a second.
struct HostSample
{
    CpuTimes cpu;
    uint32_t residentKb;
    uint32_t serial;    // counts the samples; 0 before the first

    HostSample() : residentKb(0), serial(0) {}
};

// The clocks, getrusage and the host thread around two Samplers.
class Monitor
{
public:
    Monitor();

    // Starts the host thread. Without it the host figures stay invalid.
    // @param stepped Stepped time is on (serverext.cfg stepMode 1).
    // @return False when the thread could not be started.
    bool start(bool stepped);

    enum { EventWindow = 1, LogWindow = 2 };

    // Once per tick, at the start of the tick handler, on the main thread.
    // @param handlerUs What the module's tick handler took on the tick before.
    // @return EventWindow and/or LogWindow when this tick completed that window.
    unsigned int tick(uint32_t handlerUs);

    // A level was unloaded: the load that follows is in the tick figures of the windows in progress.
    void noteLevelLoad();

    const Window& eventWindow() const { return m_event.window(); }
    const Window& logWindow() const { return m_log.window(); }

private:
    // The counters a window's figures are the difference of.
    struct Baseline
    {
        uint64_t switches;
        uint64_t faults;
        HostSample host;

        Baseline() : switches(0), faults(0) {}
    };

    void complete(Window& w, Baseline& base);
    bool latestHost(HostSample& out) const;
    void runHostThread();

    Sampler m_event;
    Sampler m_log;
    Baseline m_eventBase;
    Baseline m_logBase;
    bool m_started;
    bool m_stepped;
    // Written by the host thread only: it fills the slot that is not current, then makes it current. The main thread
    // copies the current slot, which the host thread does not write again until its next sample but one, a second
    // later at the earliest.
    HostSample m_slots[2];
    std::atomic<int> m_current;
};

} // namespace health
} // namespace serverext

#endif // SERVEREXT_HEALTH_H
