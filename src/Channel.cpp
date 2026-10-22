#include "Channel.h"

#include <fcntl.h>
#include <unistd.h>

namespace serverext {

Channel::Channel()
    : m_conn(0), m_controlBytes(0), m_overflow(false), m_hasState(false), m_logsDropped(0)
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
        if (conn == 0 || conn != m_conn)
            return;
        m_controlBytes += frame.size();
        if (m_controlBytes > kMaxControlBytes)
            m_overflow = true;
        m_control.push_back(std::vector<uint8_t>());
        m_control.back().swap(frame);
    }
    wake();
}

void Channel::sendState(uint32_t conn, std::vector<uint8_t>& frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (conn == 0 || conn != m_conn)
            return;
        m_state.swap(frame);
        m_hasState = true;
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
    if (m_commands.size() >= kMaxCommands && command.kind != Command::Disconnected)
        return false;
    m_commands.push_back(Command());
    std::swap(m_commands.back(), command);
    return true;
}

void Channel::setConnection(uint32_t conn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_conn = conn;
    // Commands of an older connection are ignored by the main thread anyway; dropping them keeps a
    // flood from a closed connection from counting against the next one. Lifecycle events stay.
    if (conn != 0)
    {
        std::deque<Command> kept;
        for (std::deque<Command>::iterator it = m_commands.begin(); it != m_commands.end(); ++it)
            if (it->kind == Command::Connected || it->kind == Command::Disconnected)
                kept.push_back(*it);
        m_commands.swap(kept);
    }
    m_control.clear();
    m_controlBytes = 0;
    m_overflow = false;
    m_hasState = false;
}

bool Channel::takeOutbound(std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t before = out.size();
    for (size_t i = 0; i < m_control.size(); ++i)
        out.insert(out.end(), m_control[i].begin(), m_control[i].end());
    m_control.clear();
    m_controlBytes = 0;
    if (m_hasState)
    {
        out.insert(out.end(), m_state.begin(), m_state.end());
        m_hasState = false;
    }
    return out.size() != before;
}

bool Channel::controlOverflow() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_overflow;
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
