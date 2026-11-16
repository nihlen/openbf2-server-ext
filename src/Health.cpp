#include "Health.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace serverext {
namespace health {

namespace {

uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }
uint16_t clamp16(uint64_t v) { return v > 0xFFFFull ? 0xFFFFu : static_cast<uint16_t>(v); }

// Reads a decimal number at text[at], skipping spaces before it. Stops at the end of the line.
bool readNumber(const char* text, size_t size, size_t& at, uint64_t& out)
{
    while (at < size && (text[at] == ' ' || text[at] == '\t'))
        ++at;
    if (at >= size || text[at] < '0' || text[at] > '9')
        return false;
    uint64_t v = 0;
    while (at < size && text[at] >= '0' && text[at] <= '9')
    {
        const uint64_t digit = static_cast<uint64_t>(text[at] - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - digit) / 10)
            return false;
        v = v * 10 + digit;
        ++at;
    }
    out = v;
    return true;
}

// Reads a whole small file. @return The bytes read, 0 on any error.
size_t readFile(const char* path, char* buf, size_t size)
{
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    size_t got = 0;
    while (got < size)
    {
        const ssize_t n = ::read(fd, buf + got, size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    return got;
}

uint64_t steadyUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t threadCpuUs()
{
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

} // namespace

Window::Window()
    : ticks(0), wallUs(0), flags(0), intervalP50Us(0), intervalP99Us(0), intervalMaxUs(0), lateTicks(0), shortTicks(0),
      cpuP50Us(0), cpuP99Us(0), cpuMaxUs(0), handlerP99Us(0), handlerMaxUs(0), involuntarySwitches(0), majorFaults(0),
      cpus(0), hostBusy(0), hostSteal(0), hostIoWait(0), residentKb(0)
{
}

ConnectionWindow::ConnectionWindow()
    : samples(0), pingLast(0), pingMin(0), pingMax(0), packetsSent(0), packetsReceived(0), bytesSent(0),
      bytesReceived(0), lostSent(0), lostSentPeak(0), lostReceived(0), lostReceivedPeak(0)
{
}

namespace {

// A rate as the engine gave it: a finite number of 0 or more, else 0.
float rate(float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; }

} // namespace

void ConnectionStats::reset()
{
    m_samples = 0;
    for (int i = 0; i < 6; ++i)
        m_sum[i] = 0;
    m_lostSentPeak = m_lostReceivedPeak = 0;
    m_pingLast = m_pingMin = m_pingMax = 0;
}

void ConnectionStats::add(const ConnectionSample& s)
{
    if (m_samples >= 255)
        return;
    const float v[6] = { rate(s.bytesSent), rate(s.bytesReceived), rate(s.packetsSent), rate(s.packetsReceived),
                         rate(s.lostSent), rate(s.lostReceived) };
    for (int i = 0; i < 6; ++i)
        m_sum[i] += v[i];
    if (v[4] > m_lostSentPeak) m_lostSentPeak = v[4];
    if (v[5] > m_lostReceivedPeak) m_lostReceivedPeak = v[5];
    const uint16_t ping = clamp16(s.ping);
    m_pingLast = ping;
    if (!m_samples || ping < m_pingMin) m_pingMin = ping;
    if (!m_samples || ping > m_pingMax) m_pingMax = ping;
    ++m_samples;
}

bool ConnectionStats::finish(ConnectionWindow& out) const
{
    if (!m_samples)
        return false;
    ConnectionWindow w;
    w.samples = static_cast<uint8_t>(m_samples);
    w.pingLast = m_pingLast;
    w.pingMin = m_pingMin;
    w.pingMax = m_pingMax;
    w.bytesSent = static_cast<float>(m_sum[0] / m_samples);
    w.bytesReceived = static_cast<float>(m_sum[1] / m_samples);
    w.packetsSent = static_cast<float>(m_sum[2] / m_samples);
    w.packetsReceived = static_cast<float>(m_sum[3] / m_samples);
    w.lostSent = static_cast<float>(m_sum[4] / m_samples);
    w.lostReceived = static_cast<float>(m_sum[5] / m_samples);
    w.lostSentPeak = m_lostSentPeak;
    w.lostReceivedPeak = m_lostReceivedPeak;
    out = w;
    return true;
}

uint32_t quantile(std::vector<uint32_t>& values, double q)
{
    if (values.empty())
        return 0;
    size_t rank = static_cast<size_t>(std::ceil(q * static_cast<double>(values.size())));
    if (rank < 1)
        rank = 1;
    if (rank > values.size())
        rank = values.size();
    std::nth_element(values.begin(), values.begin() + (rank - 1), values.end());
    return values[rank - 1];
}

Sampler::Sampler(unsigned int windowTicks)
    : m_windowTicks(windowTicks < 1 ? 1 : windowTicks > 0xFFFF ? 0xFFFF : windowTicks), m_started(false),
      m_lastWallUs(0), m_lastCpuUs(0), m_windowStartUs(0), m_pendingFlags(0)
{
    m_intervals.reserve(m_windowTicks);
    m_cpu.reserve(m_windowTicks);
    m_handler.reserve(m_windowTicks);
}

bool Sampler::add(uint64_t wallUs, uint64_t cpuUs, uint32_t handlerUs)
{
    if (!m_started)
    {
        m_started = true;
        m_lastWallUs = m_windowStartUs = wallUs;
        m_lastCpuUs = cpuUs;
        return false;
    }
    // A clock that went back counts as no time.
    m_intervals.push_back(wallUs > m_lastWallUs ? clamp32(wallUs - m_lastWallUs) : 0);
    m_cpu.push_back(cpuUs > m_lastCpuUs ? clamp32(cpuUs - m_lastCpuUs) : 0);
    m_handler.push_back(handlerUs);
    m_lastWallUs = wallUs;
    m_lastCpuUs = cpuUs;
    if (m_intervals.size() < m_windowTicks)
        return false;

    Window w;
    w.ticks = static_cast<uint16_t>(m_intervals.size());
    w.wallUs = wallUs > m_windowStartUs ? clamp32(wallUs - m_windowStartUs) : 0;
    w.flags = m_pendingFlags;
    unsigned int late = 0, quick = 0;
    for (size_t i = 0; i < m_intervals.size(); ++i)
    {
        if (m_intervals[i] > kLateIntervalUs)
            ++late;
        else if (m_intervals[i] < kShortIntervalUs)
            ++quick;
    }
    w.lateTicks = clamp16(late);
    w.shortTicks = clamp16(quick);
    w.intervalP50Us = quantile(m_intervals, 0.5);
    w.intervalP99Us = quantile(m_intervals, 0.99);
    w.intervalMaxUs = *std::max_element(m_intervals.begin(), m_intervals.end());
    w.cpuP50Us = quantile(m_cpu, 0.5);
    w.cpuP99Us = quantile(m_cpu, 0.99);
    w.cpuMaxUs = *std::max_element(m_cpu.begin(), m_cpu.end());
    w.handlerP99Us = quantile(m_handler, 0.99);
    w.handlerMaxUs = *std::max_element(m_handler.begin(), m_handler.end());
    m_window = w;

    m_intervals.clear();
    m_cpu.clear();
    m_handler.clear();
    m_windowStartUs = wallUs;
    m_pendingFlags = 0;
    return true;
}

bool parseProcStat(const char* text, size_t size, CpuTimes& out)
{
    CpuTimes t;
    bool total = false;
    size_t at = 0;
    while (at < size)
    {
        size_t end = at;
        while (end < size && text[end] != '\n')
            ++end;
        if (end - at >= 4 && std::memcmp(text + at, "cpu", 3) == 0)
        {
            if (text[at + 3] == ' ' && !total)
            {
                // cpu  user nice system idle iowait irq softirq steal [guest guest_nice]; guest time is in user
                uint64_t f[8];
                size_t p = at + 3;
                for (int i = 0; i < 8; ++i)
                    if (!readNumber(text, end, p, f[i]))
                        return false;
                t.busy = f[0] + f[1] + f[2] + f[5] + f[6];
                t.ioWait = f[4];
                t.steal = f[7];
                t.total = t.busy + f[3] + f[4] + f[7];
                total = true;
            }
            else if (text[at + 3] >= '0' && text[at + 3] <= '9' && t.cpus < 0xFFFF)
                ++t.cpus;
        }
        at = end + 1;
    }
    if (!total)
        return false;
    out = t;
    return true;
}

bool parseStatm(const char* text, size_t size, uint64_t& residentPages)
{
    size_t at = 0;
    uint64_t total = 0, resident = 0;
    if (!readNumber(text, size, at, total) || !readNumber(text, size, at, resident))
        return false;
    residentPages = resident;
    return true;
}

uint16_t share(uint64_t part, uint64_t whole)
{
    if (!whole)
        return 0;
    if (part >= whole)
        return 10000;
    return static_cast<uint16_t>((part * 10000ull + whole / 2) / whole);
}

Monitor::Monitor()
    : m_event(kEventWindowTicks), m_log(kLogWindowTicks), m_started(false), m_stepped(false), m_current(-1)
{
}

bool Monitor::start(bool stepped)
{
    m_stepped = stepped;
    // Lives for the process, like the bot channel's I/O thread: the loader has no unload path.
    try
    {
        std::thread t(&Monitor::runHostThread, this);
        t.detach();
    }
    catch (...)
    {
        return false;
    }
    return true;
}

void Monitor::runHostThread()
{
    static char buf[65536];   // this thread only
    const long pageKb = ::sysconf(_SC_PAGESIZE) / 1024;
    uint32_t serial = 0;
    for (;;)
    {
        HostSample s;
        size_t n = readFile("/proc/stat", buf, sizeof(buf));
        const bool cpuOk = n > 0 && parseProcStat(buf, n, s.cpu);
        uint64_t pages = 0;
        n = readFile("/proc/self/statm", buf, sizeof(buf));
        if (n > 0 && parseStatm(buf, n, pages))
            s.residentKb = clamp32(pages * static_cast<uint64_t>(pageKb > 0 ? pageKb : 4));
        if (cpuOk)
        {
            s.serial = ++serial;
            const int next = m_current.load(std::memory_order_relaxed) == 0 ? 1 : 0;
            m_slots[next] = s;
            m_current.store(next, std::memory_order_release);
        }
        timespec second = { 1, 0 };
        nanosleep(&second, 0);
    }
}

bool Monitor::latestHost(HostSample& out) const
{
    const int current = m_current.load(std::memory_order_acquire);
    if (current < 0)
        return false;
    out = m_slots[current];
    return true;
}

void Monitor::noteLevelLoad()
{
    m_event.noteLevelLoad();
    m_log.noteLevelLoad();
}

void Monitor::complete(Window& w, Baseline& base)
{
    if (m_stepped)
        w.flags |= kFlagStepped;
    rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) == 0)
    {
        const uint64_t switches = static_cast<uint64_t>(ru.ru_nivcsw);
        const uint64_t faults = static_cast<uint64_t>(ru.ru_majflt);
        w.involuntarySwitches = switches > base.switches ? clamp32(switches - base.switches) : 0;
        w.majorFaults = faults > base.faults ? clamp32(faults - base.faults) : 0;
        base.switches = switches;
        base.faults = faults;
    }
    HostSample now;
    if (latestHost(now))
    {
        w.cpus = now.cpu.cpus;
        w.residentKb = now.residentKb;
        // Valid when the host thread took a new sample since the window began and the counters moved on.
        if (base.host.serial && now.serial != base.host.serial && now.cpu.total > base.host.cpu.total)
        {
            const uint64_t total = now.cpu.total - base.host.cpu.total;
            w.hostBusy = share(now.cpu.busy > base.host.cpu.busy ? now.cpu.busy - base.host.cpu.busy : 0, total);
            w.hostSteal = share(now.cpu.steal > base.host.cpu.steal ? now.cpu.steal - base.host.cpu.steal : 0, total);
            w.hostIoWait = share(now.cpu.ioWait > base.host.cpu.ioWait ? now.cpu.ioWait - base.host.cpu.ioWait : 0, total);
            w.flags |= kFlagHostValid;
        }
        base.host = now;
    }
}

unsigned int Monitor::tick(uint32_t handlerUs)
{
    const uint64_t wall = steadyUs();
    const uint64_t cpu = threadCpuUs();
    if (!m_started)
    {
        // The starting point of the first windows' switch, fault and host figures.
        m_started = true;
        Window unused;
        complete(unused, m_eventBase);
        m_logBase = m_eventBase;
    }
    unsigned int done = 0;
    if (m_event.add(wall, cpu, handlerUs))
    {
        complete(m_event.window(), m_eventBase);
        done |= EventWindow;
    }
    if (m_log.add(wall, cpu, handlerUs))
    {
        complete(m_log.window(), m_logBase);
        done |= LogWindow;
    }
    return done;
}

} // namespace health
} // namespace serverext
