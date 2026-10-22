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
#include <vector>

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
    IoThread(Channel* channel, int listenFd, int maxConnections)
        : m_channel(channel), m_listen(listenFd), m_maxConnections(maxConnections), m_nextConn(0),
          m_buffer(kReadChunk) {}

    void run()
    {
        while (true)
            step();
    }

private:
    struct Client
    {
        int fd;
        uint32_t conn;
        Session* session;
        std::vector<uint8_t> write;
        size_t writePos;

        bool pendingWrite() const { return writePos < write.size(); }
    };

    void step()
    {
        std::vector<pollfd> fds(2 + m_clients.size());
        fds[0].fd = m_channel->wakeReadFd(); fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = m_listen; fds[1].events = POLLIN; fds[1].revents = 0;
        for (size_t i = 0; i < m_clients.size(); ++i)
        {
            Client& c = m_clients[i];
            fds[2 + i].fd = c.fd;
            fds[2 + i].events = POLLIN | (c.pendingWrite() ? POLLOUT : 0);
            fds[2 + i].revents = 0;
        }

        const int r = ::poll(&fds[0], fds.size(), kPollMs);
        if (r < 0 && errno != EINTR)
            ::usleep(10000);   // should not happen; never spin

        if (fds[0].revents & POLLIN)
        {
            char drain[256];
            while (::read(fds[0].fd, drain, sizeof(drain)) > 0) {}
        }
        flushLogs();

        // A closed client gets fd -1 and is swept below, so the indices into fds stay valid.
        const size_t polled = m_clients.size();
        for (size_t i = 0; i < polled; ++i)
        {
            Client& c = m_clients[i];
            if (c.fd >= 0 && (fds[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                readClient(c);
            if (c.fd >= 0 && m_channel->controlOverflow(c.conn))
                closeClient(c, "the sidecar is not reading; outbound control queue exceeded 8 MiB", 0, 0);
            if (c.fd >= 0)
                writeClient(c);
        }
        for (size_t i = 0; i < m_clients.size();)
        {
            if (m_clients[i].fd < 0)
                m_clients.erase(m_clients.begin() + i);
            else
                ++i;
        }

        if (fds[1].revents & POLLIN)
            acceptClient();
    }

    void acceptClient()
    {
        sockaddr_in peer;
        socklen_t len = sizeof(peer);
        const int fd = ::accept(m_listen, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0)
            return;
        char addr[32];
        ::inet_ntop(AF_INET, &peer.sin_addr, addr, sizeof(addr));
        if (static_cast<int>(m_clients.size()) >= m_maxConnections)
        {
            char line[128];
            std::snprintf(line, sizeof(line), "refused a connection from %s: %d already open (maxConnections)", addr,
                          m_maxConnections);
            log(line);
            ::close(fd);
            return;
        }
        setNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Client c;
        c.fd = fd;
        c.conn = ++m_nextConn;
        c.session = new Session(m_channel, c.conn);
        c.writePos = 0;
        m_clients.push_back(c);
        m_channel->openConnection(c.conn);
        char line[96];
        std::snprintf(line, sizeof(line), "connection %u from %s:%u", c.conn, addr, ntohs(peer.sin_port));
        log(line);
        Command cmd;
        cmd.kind = Command::Connected;
        cmd.conn = c.conn;
        m_channel->pushCommand(cmd);
    }

    void readClient(Client& c)
    {
        size_t total = 0;
        while (c.fd >= 0 && total < kMaxReadPerWake)
        {
            const ssize_t got = ::recv(c.fd, &m_buffer[0], m_buffer.size(), 0);
            if (got > 0)
            {
                total += static_cast<size_t>(got);
                std::vector<uint8_t> reply;
                if (!c.session->onBytes(&m_buffer[0], static_cast<size_t>(got), reply))
                {
                    closeClient(c, c.session->closeReason(), reply.empty() ? 0 : &reply[0], reply.size());
                    return;
                }
                continue;
            }
            if (got == 0)
            {
                closeClient(c, "the sidecar closed the connection", 0, 0);
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                closeClient(c, errnoText("recv"), 0, 0);
            return;
        }
    }

    void writeClient(Client& c)
    {
        if (!c.pendingWrite())
        {
            c.write.clear();
            c.writePos = 0;
            if (!m_channel->takeOutbound(c.conn, c.write))
                return;
        }
        while (c.pendingWrite())
        {
            const ssize_t sent = ::send(c.fd, &c.write[c.writePos], c.write.size() - c.writePos, MSG_NOSIGNAL);
            if (sent > 0)
            {
                c.writePos += static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            closeClient(c, errnoText("send"), 0, 0);
            return;
        }
    }

    // Sends what is left of the current frame plus an optional Error frame, best effort and without
    // blocking, then closes and tells the main thread.
    void closeClient(Client& c, const std::string& reason, const uint8_t* finalFrame, size_t finalSize)
    {
        if (finalSize)
        {
            c.write.insert(c.write.end(), finalFrame, finalFrame + finalSize);
            while (c.pendingWrite())
            {
                const ssize_t sent = ::send(c.fd, &c.write[c.writePos], c.write.size() - c.writePos,
                                            MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent <= 0)
                    break;
                c.writePos += static_cast<size_t>(sent);
            }
        }
        ::shutdown(c.fd, SHUT_RDWR);
        ::close(c.fd);
        c.fd = -1;
        char line[64];
        std::snprintf(line, sizeof(line), "connection %u closed after %llu frames: ", c.conn,
                      static_cast<unsigned long long>(c.session ? c.session->frames() : 0));
        log(line + reason);
        delete c.session;
        c.session = 0;
        c.write.clear();
        c.writePos = 0;
        m_channel->closeConnection(c.conn);
        Command cmd;
        cmd.kind = Command::Disconnected;
        cmd.conn = c.conn;
        m_channel->pushCommand(cmd);
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
    int m_maxConnections;
    uint32_t m_nextConn;
    std::vector<Client> m_clients;
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
    if (::listen(fd, 8) != 0)
    {
        error = errnoText("listen");
        ::close(fd);
        return false;
    }
    setNonBlocking(fd);

    // Lives for the process: the loader has no unload path, and a joinable std::thread destroyed at
    // exit would call std::terminate.
    IoThread* io = new IoThread(channel, fd, config.maxConnections);
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
