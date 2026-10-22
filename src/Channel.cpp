#include "Channel.h"

#include <fcntl.h>
#include <unistd.h>

namespace serverext {

Channel::Channel()
    : m_logsDropped(0)
{
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

void Channel::wake()
{
    if (m_wake[1] >= 0)
    {
        const char b = 1;
        // Non-blocking; a full pipe already guarantees a wake-up.
        ssize_t ignored = ::write(m_wake[1], &b, 1);
        (void)ignored;
    }
}

void Channel::drainCommands(std::deque<Command>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_commands);
}

void Channel::sendControl(uint32_t conn, std::vector<uint8_t>& frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<uint32_t, Outbound>::iterator it = m_out.find(conn);
        if (it == m_out.end())
            return;
        Outbound& o = it->second;
        o.controlBytes += frame.size();
        if (o.controlBytes > kMaxControlBytes)
            o.overflow = true;
        o.control.push_back(std::vector<uint8_t>());
        o.control.back().swap(frame);
    }
    wake();
}

void Channel::sendLatest(uint32_t conn, Slot slot, std::vector<uint8_t>& frame)
{
    if (slot < 0 || slot >= SlotCount)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<uint32_t, Outbound>::iterator it = m_out.find(conn);
        if (it == m_out.end())
            return;
        it->second.latest[slot].swap(frame);
        it->second.hasLatest[slot] = true;
    }
    wake();
}

void Channel::log(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logs.size() >= kMaxLogLines)
        {
            ++m_logsDropped;
            return;
        }
        m_logs.push_back(line);
    }
    wake();
}

bool Channel::pushCommand(Command& command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_commands.size() >= kMaxCommands && command.kind != Command::Disconnected &&
        command.kind != Command::Connected)
        return false;
    m_commands.push_back(Command());
    std::swap(m_commands.back(), command);
    return true;
}

void Channel::openConnection(uint32_t conn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_out[conn] = Outbound();
}

void Channel::closeConnection(uint32_t conn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_out.erase(conn);
    // The main thread ignores commands of a closed connection anyway; dropping them keeps a flood
    // from a closed connection from counting against the others. Lifecycle events stay.
    std::deque<Command> kept;
    for (std::deque<Command>::iterator it = m_commands.begin(); it != m_commands.end(); ++it)
        if (it->conn != conn || it->kind == Command::Connected || it->kind == Command::Disconnected)
            kept.push_back(*it);
    m_commands.swap(kept);
}

bool Channel::takeOutbound(uint32_t conn, std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<uint32_t, Outbound>::iterator it = m_out.find(conn);
    if (it == m_out.end())
        return false;
    Outbound& o = it->second;
    const size_t before = out.size();
    for (size_t i = 0; i < o.control.size(); ++i)
        out.insert(out.end(), o.control[i].begin(), o.control[i].end());
    o.control.clear();
    o.controlBytes = 0;
    for (int s = 0; s < SlotCount; ++s)
    {
        if (!o.hasLatest[s])
            continue;
        out.insert(out.end(), o.latest[s].begin(), o.latest[s].end());
        o.hasLatest[s] = false;
    }
    return out.size() != before;
}

bool Channel::controlOverflow(uint32_t conn) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<uint32_t, Outbound>::const_iterator it = m_out.find(conn);
    return it != m_out.end() && it->second.overflow;
}

size_t Channel::openConnections() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_out.size();
}

size_t Channel::drainLogs(std::deque<std::string>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_logs);
    const size_t dropped = m_logsDropped;
    m_logsDropped = 0;
    return dropped;
}

} // namespace serverext
