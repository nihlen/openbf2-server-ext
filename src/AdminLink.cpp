#include "AdminLink.h"

#include <fcntl.h>
#include <unistd.h>

namespace serverext {
namespace admin {

AdminLink::AdminLink(size_t queueBytes)
    : m_queueBytes(queueBytes), m_session(0), m_queued(0), m_overloaded(false), m_dropped(0)
{
    for (int i = 0; i < SlotCount; ++i)
        m_hasLatest[i] = false;
    m_wake[0] = m_wake[1] = -1;
    if (::pipe(m_wake) == 0)
    {
        for (int i = 0; i < 2; ++i)
        {
            ::fcntl(m_wake[i], F_SETFL, ::fcntl(m_wake[i], F_GETFL) | O_NONBLOCK);
            ::fcntl(m_wake[i], F_SETFD, FD_CLOEXEC);
        }
    }
}

AdminLink::~AdminLink()
{
    for (int i = 0; i < 2; ++i)
        if (m_wake[i] >= 0)
            ::close(m_wake[i]);
}

void AdminLink::wake()
{
    if (m_wake[1] >= 0)
    {
        const char b = 1;
        ssize_t ignored = ::write(m_wake[1], &b, 1);   // non-blocking; a full pipe already wakes
        (void)ignored;
    }
}

void AdminLink::drainCommands(std::deque<Command>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_commands);
}

void AdminLink::sendReply(uint32_t session, std::vector<uint8_t>& frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!session || session != m_session)
            return;
        m_queued += frame.size();
        if (m_queued >= 4 * m_queueBytes)
            m_overloaded = true;
        m_queue.push_back(std::vector<uint8_t>());
        m_queue.back().swap(frame);
    }
    wake();
}

bool AdminLink::sendEvent(uint32_t session, std::vector<uint8_t>& frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!session || session != m_session)
            return false;
        if (m_queued + frame.size() > m_queueBytes)
        {
            ++m_dropped;
            return false;
        }
        m_queued += frame.size();
        m_queue.push_back(std::vector<uint8_t>());
        m_queue.back().swap(frame);
    }
    wake();
    return true;
}

void AdminLink::sendLatest(uint32_t session, Slot slot, std::vector<uint8_t>& frame)
{
    if (slot < 0 || slot >= SlotCount)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!session || session != m_session)
            return;
        m_latest[slot].swap(frame);
        m_hasLatest[slot] = true;
    }
    wake();
}

void AdminLink::openSession(uint32_t session)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session = session;
    m_queue.clear();
    m_queued = 0;
    m_overloaded = false;
    for (int i = 0; i < SlotCount; ++i)
        m_hasLatest[i] = false;
}

void AdminLink::closeSession()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session = 0;
    m_queue.clear();
    m_queued = 0;
    m_overloaded = false;
    for (int i = 0; i < SlotCount; ++i)
        m_hasLatest[i] = false;
}

bool AdminLink::pushCommand(Command& command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_commands.size() >= kMaxCommands && command.type != Command::LinkUp && command.type != Command::LinkDown)
        return false;
    m_commands.push_back(Command());
    std::swap(m_commands.back(), command);
    return true;
}

bool AdminLink::takeOutbound(std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t before = out.size();
    for (size_t i = 0; i < m_queue.size(); ++i)
        out.insert(out.end(), m_queue[i].begin(), m_queue[i].end());
    m_queue.clear();
    m_queued = 0;
    for (int s = 0; s < SlotCount; ++s)
    {
        if (!m_hasLatest[s])
            continue;
        out.insert(out.end(), m_latest[s].begin(), m_latest[s].end());
        m_hasLatest[s] = false;
    }
    return out.size() != before;
}

bool AdminLink::overloaded() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_overloaded;
}

uint64_t AdminLink::takeDroppedEvents()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t n = m_dropped;
    m_dropped = 0;
    return n;
}

} // namespace admin
} // namespace serverext
