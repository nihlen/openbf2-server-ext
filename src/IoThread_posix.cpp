// POSIX implementation of the bot channel's socket thread. A Windows build adds a Winsock
// counterpart with the same startIoThread contract.

#include "IoThread.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "Session.h"

namespace serverext {

namespace {

const size_t kReadChunk = 64 * 1024;
const size_t kMaxReadPerWake = 1024 * 1024;   // fairness: then poll again
const int kPollMs = 100;                      // log flush cadence when idle

void setNonBlocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string errnoText(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

class IoThread
{
public:
    IoThread(Channel* channel, int listenFd)
        : m_channel(channel), m_listen(listenFd), m_client(-1), m_conn(0), m_session(0), m_writePos(0),
          m_buffer(kReadChunk) {}

    void run()
    {
        while (true)
            step();
    }

private:
    void step()
    {
        pollfd fds[3];
        int n = 0;
        fds[n].fd = m_channel->wakeReadFd(); fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        fds[n].fd = m_listen; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        const int clientIndex = n;
        if (m_client >= 0)
        {
            fds[n].fd = m_client;
            fds[n].events = POLLIN | (pendingWrite() ? POLLOUT : 0);
            fds[n].revents = 0;
            ++n;
        }

        const int r = ::poll(fds, n, kPollMs);
        if (r < 0 && errno != EINTR)
            ::usleep(10000);   // should not happen; never spin

        if (fds[0].revents & POLLIN)
        {
            char drain[256];
            while (::read(fds[0].fd, drain, sizeof(drain)) > 0) {}
        }
        flushLogs();

        if (fds[1].revents & POLLIN)
            acceptClient();

        if (m_client >= 0 && clientIndex < n && (fds[clientIndex].revents & (POLLIN | POLLHUP | POLLERR)))
            readClient();

        if (m_client >= 0 && m_channel->controlOverflow())
            closeClient("the sidecar is not reading; outbound control queue exceeded 8 MiB", 0, 0);

        if (m_client >= 0)
            writeClient();
    }

    bool pendingWrite() const { return m_writePos < m_write.size(); }

    void acceptClient()
    {
        sockaddr_in peer;
        socklen_t len = sizeof(peer);
        const int fd = ::accept(m_listen, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0)
            return;
        char addr[32];
        ::inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
        if (m_client >= 0)
        {
            log(std::string("refused a second connection from ") + addr + "; one sidecar at a time in v0");
            ::close(fd);
            return;
        }
        setNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        m_client = fd;
        ++m_conn;
        m_session = new Session(m_channel, m_conn);
        m_write.clear();
        m_writePos = 0;
        m_channel->setConnection(m_conn);
        char line[96];
        std::snprintf(line, sizeof(line), "connection %u from %s:%u", m_conn, addr, ntohs(peer.sin_port));
        log(line);
        Command c;
        c.kind = Command::Connected;
        c.conn = m_conn;
        m_channel->pushCommand(c);
    }

    void readClient()
    {
        size_t total = 0;
        while (m_client >= 0 && total < kMaxReadPerWake)
        {
            const ssize_t got = ::recv(m_client, &m_buffer[0], m_buffer.size(), 0);
            if (got > 0)
            {
                total += static_cast<size_t>(got);
                std::vector<uint8_t> reply;
                if (!m_session->onBytes(&m_buffer[0], static_cast<size_t>(got), reply))
                {
                    closeClient(m_session->closeReason(), reply.empty() ? 0 : &reply[0], reply.size());
                    return;
                }
                continue;
            }
            if (got == 0)
            {
                closeClient("the sidecar closed the connection", 0, 0);
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                closeClient(errnoText("recv"), 0, 0);
            return;
        }
    }

    void writeClient()
    {
        if (!pendingWrite())
        {
            m_write.clear();
            m_writePos = 0;
            if (!m_channel->takeOutbound(m_write))
                return;
        }
        while (pendingWrite())
        {
            const ssize_t sent = ::send(m_client, &m_write[m_writePos], m_write.size() - m_writePos, MSG_NOSIGNAL);
            if (sent > 0)
            {
                m_writePos += static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            closeClient(errnoText("send"), 0, 0);
            return;
        }
    }

    // Sends what is left of the current frame plus an optional Error frame, best effort and without
    // blocking, then closes and tells the main thread.
    void closeClient(const std::string& reason, const uint8_t* finalFrame, size_t finalSize)
    {
        if (finalSize)
        {
            m_write.insert(m_write.end(), finalFrame, finalFrame + finalSize);
            while (pendingWrite())
            {
                const ssize_t sent = ::send(m_client, &m_write[m_writePos], m_write.size() - m_writePos,
                                            MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent <= 0)
                    break;
                m_writePos += static_cast<size_t>(sent);
            }
        }
        ::shutdown(m_client, SHUT_RDWR);
        ::close(m_client);
        m_client = -1;
        char line[64];
        std::snprintf(line, sizeof(line), "connection %u closed after %llu frames: ", m_conn,
                      static_cast<unsigned long long>(m_session ? m_session->frames() : 0));
        log(line + reason);
        delete m_session;
        m_session = 0;
        m_write.clear();
        m_writePos = 0;
        m_channel->setConnection(0);
        Command c;
        c.kind = Command::Disconnected;
        c.conn = m_conn;
        m_channel->pushCommand(c);
    }

    void log(const std::string& line) { m_channel->log(line); }

    void flushLogs()
    {
        const size_t dropped = m_channel->drainLogs(m_logs);
        for (size_t i = 0; i < m_logs.size(); ++i)
            std::fprintf(stderr, "[serverext] %s\n", m_logs[i].c_str());
        if (dropped)
            std::fprintf(stderr, "[serverext] (%u log lines dropped)\n", static_cast<unsigned>(dropped));
        if (!m_logs.empty() || dropped)
            std::fflush(stderr);
        m_logs.clear();
    }

    Channel* m_channel;
    int m_listen;
    int m_client;
    uint32_t m_conn;
    Session* m_session;
    std::vector<uint8_t> m_write;
    size_t m_writePos;
    std::vector<uint8_t> m_buffer;
    std::deque<std::string> m_logs;
};

} // namespace

bool startIoThread(const Config& config, Channel* channel, std::string& error)
{
    if (channel->wakeReadFd() < 0)
    {
        error = "could not create the wake-up pipe";
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error = errnoText("socket");
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config.listenPort));
    if (::inet_pton(AF_INET, config.listenHost.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        error = "invalid listen address " + config.listenHost;
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        error = errnoText("bind");
        ::close(fd);
        return false;
    }
    if (::listen(fd, 4) != 0)
    {
        error = errnoText("listen");
        ::close(fd);
        return false;
    }
    setNonBlocking(fd);

    // Lives for the process: the loader has no unload path, and a joinable std::thread destroyed at
    // exit would call std::terminate.
    IoThread* io = new IoThread(channel, fd);
    try
    {
        std::thread t(&IoThread::run, io);
        t.detach();
    }
    catch (...)
    {
        error = "could not start the I/O thread";
        return false;
    }
    return true;
}

} // namespace serverext
