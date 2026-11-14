// POSIX implementation of the admin channel's I/O thread (AdminIo.h).

#include "AdminIo.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "AdminCrypto.h"
#include "AdminSession.h"

namespace serverext {
namespace admin {

namespace {

const size_t kReadChunk = 64 * 1024;
const size_t kMaxReadPerWake = 1024 * 1024;
const size_t kRefillBelow = 256 * 1024;     // take more from the link only below this backlog
const size_t kTlsWriteChunk = 16 * 1024;
const int kPollMs = 100;
const int64_t kDropLogMs = 10000;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string errnoText(const char* what, int err)
{
    return std::string(what) + ": " + std::strerror(err);
}

std::string sslErrors()
{
    std::string s;
    unsigned long e;
    while ((e = ERR_get_error()) != 0)
    {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!s.empty())
            s += "; ";
        s += buf;
    }
    return s.empty() ? std::string("no OpenSSL error") : s;
}

int verifyCallback(int preverify, X509_STORE_CTX* store)
{
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    AdminIo* io = ssl ? static_cast<AdminIo*>(SSL_get_app_data(ssl)) : 0;
    return io ? io->verifyPeer(preverify, store) : 0;
}

bool isLoopback(const sockaddr* a)
{
    if (a->sa_family == AF_INET)
        return (ntohl(reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr) >> 24) == 127;
    if (a->sa_family == AF_INET6)
    {
        const in6_addr& v6 = reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr;
        return IN6_IS_ADDR_LOOPBACK(&v6);
    }
    return false;
}

} // namespace

AdminIo::AdminIo(const AdminIoConfig& config, AdminLink* link, LogFn log)
    : m_config(config), m_link(link), m_log(log), m_ctx(0), m_ssl(0), m_rbio(0), m_wbio(0), m_stop(false),
      m_authenticatedLinks(0), m_attempts(0), m_state(Idle), m_fd(-1), m_nextAttempt(0), m_connectStart(0),
      m_lastReceive(0), m_lastDropLog(0), m_droppedSinceLog(0), m_pingOutstanding(false), m_pingId(0),
      m_failures(0), m_pinChecked(false), m_pinOk(false), m_counted(false), m_sessionCounter(0), m_session(0), m_appPos(0),
      m_netPos(0), m_buffer(kReadChunk)
{
}

AdminIo::~AdminIo()
{
    stop();
    if (m_ctx)
        SSL_CTX_free(static_cast<SSL_CTX*>(m_ctx));
}

bool AdminIo::start(std::string& error)
{
    if (m_link->wakeReadFd() < 0)
    {
        error = "could not create the admin wake-up pipe";
        return false;
    }
    if (m_config.tls)
    {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx)
        {
            error = "SSL_CTX_new: " + sslErrors();
            return false;
        }
        if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1)
        {
            error = "OpenSSL has no TLS 1.3: " + sslErrors();
            SSL_CTX_free(ctx);
            return false;
        }
        // No CA store and no host name check: the pinned SPKI is the only trust.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verifyCallback);
        SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        m_ctx = ctx;
    }
    m_stop = false;
    try
    {
        m_thread = std::thread(&AdminIo::run, this);
    }
    catch (...)
    {
        error = "could not start the admin I/O thread";
        return false;
    }
    return true;
}

void AdminIo::stop()
{
    if (!m_thread.joinable())
        return;
    m_stop = true;
    m_link->wake();
    m_thread.join();
    if (m_state != Idle)
        closeLink("stopped");
}

std::string AdminIo::lastCloseReason() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_lastClose;
}

std::string AdminIo::lastPeerPin() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_peerPin;
}

int AdminIo::verifyPeer(int, void* storeCtx)
{
    X509_STORE_CTX* store = static_cast<X509_STORE_CTX*>(storeCtx);
    // Only the leaf matters: WebAdmin's certificate is self-signed and pinned, so OpenSSL's own
    // chain verdict (always "self-signed") is ignored.
    if (X509_STORE_CTX_get_error_depth(store) != 0)
        return 1;
    X509* cert = X509_STORE_CTX_get_current_cert(store);
    X509_PUBKEY* key = cert ? X509_get_X509_PUBKEY(cert) : 0;
    const int len = key ? i2d_X509_PUBKEY(key, 0) : 0;
    if (len <= 0)
    {
        m_pinChecked = true;
        m_pinOk = false;
        return 0;
    }
    std::vector<uint8_t> der(static_cast<size_t>(len));
    unsigned char* p = &der[0];
    i2d_X509_PUBKEY(key, &p);
    uint8_t digest[32];
    sha256(&der[0], der.size(), digest);
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_peerPin = toHex(digest, sizeof(digest));
    }
    m_pinChecked = true;
    m_pinOk = m_config.pin.size() == 32 && equalConstantTime(digest, &m_config.pin[0], 32);
    if (m_pinOk)
        X509_STORE_CTX_set_error(store, X509_V_OK);
    return m_pinOk ? 1 : 0;
}

void AdminIo::run()
{
    while (!m_stop)
        step();
}

void AdminIo::step()
{
    const int64_t now = nowMs();
    if (m_state == Idle && now >= m_nextAttempt)
        beginConnect(now);

    const bool authenticated = m_session && m_session->authenticated();
    if (m_state != Idle && !authenticated && now - m_connectStart > m_config.handshakeTimeoutMs)
    {
        char why[96];
        std::snprintf(why, sizeof(why), "the handshake did not finish within %d ms", m_config.handshakeTimeoutMs);
        closeLink(why);
    }
    if (m_state == Open && authenticated)
    {
        if (now - m_lastReceive > m_config.silenceLimitMs)
        {
            char why[96];
            std::snprintf(why, sizeof(why), "nothing received from WebAdmin for %d ms", m_config.silenceLimitMs);
            closeLink(why);
        }
        else if (now - m_lastReceive > m_config.pingAfterMs && !m_pingOutstanding)
            sendPing(now);
        else if (m_link->overloaded())
            closeLink("the outbound queue reached four times adminQueueBytes; WebAdmin is not reading");
    }

    const uint64_t dropped = m_link->takeDroppedEvents();
    m_droppedSinceLog += dropped;
    if (m_droppedSinceLog && now - m_lastDropLog >= kDropLogMs)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "admin: %llu events dropped because the outbound queue was full (WebAdmin will resync)",
                      static_cast<unsigned long long>(m_droppedSinceLog));
        log(line);
        m_droppedSinceLog = 0;
        m_lastDropLog = now;
    }

    if (m_state == Open && m_session && m_session->authenticated() && m_appOut.size() - m_appPos < kRefillBelow &&
        m_netOut.size() - m_netPos < kRefillBelow)
    {
        if (m_appPos == m_appOut.size())
        {
            m_appOut.clear();
            m_appPos = 0;
        }
        m_link->takeOutbound(m_appOut);
    }
    pump(now);
    writeSocket();

    pollfd fds[2];
    fds[0].fd = m_link->wakeReadFd(); fds[0].events = POLLIN; fds[0].revents = 0;
    int nfds = 1;
    if (m_fd >= 0)
    {
        fds[1].fd = m_fd;
        fds[1].events = static_cast<short>(
            (m_state == Connecting ? POLLOUT : POLLIN) | (m_netPos < m_netOut.size() ? POLLOUT : 0));
        fds[1].revents = 0;
        nfds = 2;
    }
    int timeout = kPollMs;
    if (m_state == Idle && m_nextAttempt - now < timeout)
        timeout = m_nextAttempt > now ? static_cast<int>(m_nextAttempt - now) : 0;
    const int r = ::poll(fds, nfds, timeout);
    if (r < 0 && errno != EINTR)
        ::usleep(10000);
    if (fds[0].revents & POLLIN)
    {
        char drain[256];
        while (::read(fds[0].fd, drain, sizeof(drain)) > 0) {}
    }
    if (nfds == 2 && m_fd >= 0)
    {
        const int64_t t = nowMs();
        if (m_state == Connecting && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP)))
        {
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err)
                closeLink(errnoText("connect", err));
            else
                onConnected(t);
        }
        else if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
            readSocket(t);
        if (m_state != Idle)
        {
            pump(t);
            writeSocket();
        }
    }
}

void AdminIo::beginConnect(int64_t now)
{
    ++m_attempts;
    m_connectStart = now;
    char port[16];
    std::snprintf(port, sizeof(port), "%d", m_config.port);
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = 0;
    // A blocking lookup, on this thread only: the game thread and the bot channel never wait on it.
    const int gai = ::getaddrinfo(m_config.host.c_str(), port, &hints, &res);
    if (gai != 0 || !res)
    {
        m_state = Connecting;   // so closeLink schedules the retry
        closeLink(std::string("cannot resolve ") + m_config.host + ": " + gai_strerror(gai));
        return;
    }
    if (!m_config.tls && !isLoopback(res->ai_addr))
    {
        ::freeaddrinfo(res);
        m_state = Connecting;
        closeLink("adminTls 0 and " + m_config.host + " is not a loopback address; refusing to send in the clear");
        return;
    }
    const int fd = ::socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
    {
        const int err = errno;
        ::freeaddrinfo(res);
        m_state = Connecting;
        closeLink(errnoText("socket", err));
        return;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m_fd = fd;
    m_state = Connecting;
    const int c = ::connect(fd, res->ai_addr, res->ai_addrlen);
    const int err = errno;
    ::freeaddrinfo(res);
    if (c == 0)
        onConnected(nowMs());
    else if (err != EINPROGRESS)
        closeLink(errnoText("connect", err));
}

void AdminIo::onConnected(int64_t now)
{
    if (!m_config.tls)
    {
        startSession(now);
        return;
    }
    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(m_ctx));
    BIO* rbio = ssl ? BIO_new(BIO_s_mem()) : 0;
    BIO* wbio = rbio ? BIO_new(BIO_s_mem()) : 0;
    if (!ssl || !rbio || !wbio)
    {
        if (rbio) BIO_free(rbio);
        if (ssl) SSL_free(ssl);
        closeLink("cannot create the TLS session: " + sslErrors());
        return;
    }
    SSL_set_bio(ssl, rbio, wbio);   // ssl owns both now
    SSL_set_connect_state(ssl);
    SSL_set_app_data(ssl, this);
    m_ssl = ssl;
    m_rbio = rbio;
    m_wbio = wbio;
    m_pinChecked = m_pinOk = false;
    m_state = Handshaking;
}

void AdminIo::startSession(int64_t now)
{
    m_session = new AdminSession(m_link, ++m_sessionCounter, m_config.serverId, m_config.key, m_config.build);
    m_state = Open;
    m_counted = false;
    m_lastReceive = now;
    m_pingOutstanding = false;
    if (!m_session->start(m_appOut))
        closeLink(m_session->closeReason());
}

void AdminIo::readSocket(int64_t now)
{
    size_t total = 0;
    while (m_fd >= 0 && total < kMaxReadPerWake)
    {
        const ssize_t got = ::recv(m_fd, &m_buffer[0], m_buffer.size(), 0);
        if (got > 0)
        {
            total += static_cast<size_t>(got);
            m_lastReceive = now;
            m_pingOutstanding = false;
            if (m_ssl)
            {
                BIO_write(static_cast<BIO*>(m_rbio), &m_buffer[0], static_cast<int>(got));
                continue;
            }
            if (m_session && !m_session->onBytes(&m_buffer[0], static_cast<size_t>(got), m_appOut))
            {
                // Send the final Error (if any) best effort, then close.
                pump(now);
                writeSocket();
                closeLink(m_session->closeReason());
                return;
            }
            noteAuthentication();
            continue;
        }
        if (got == 0)
        {
            closeLink("WebAdmin closed the connection");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            closeLink(errnoText("recv", errno));
        return;
    }
}

void AdminIo::pump(int64_t now)
{
    if (m_state == Idle)
        return;
    SSL* ssl = static_cast<SSL*>(m_ssl);
    if (!ssl)
    {
        // Plain mode: plaintext goes straight to the socket.
        if (m_appPos < m_appOut.size())
        {
            m_netOut.insert(m_netOut.end(), m_appOut.begin() + m_appPos, m_appOut.end());
            m_appOut.clear();
            m_appPos = 0;
        }
        return;
    }

    if (m_state == Handshaking)
    {
        ERR_clear_error();
        const int r = SSL_do_handshake(ssl);
        if (r == 1)
        {
            if (!m_pinChecked || !m_pinOk)
            {
                closeLink("WebAdmin's certificate does not match adminCertSha256 (peer SPKI sha256 " + lastPeerPin() + ")");
                return;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "admin: TLS up (%s, %s), certificate pin matches", SSL_get_version(ssl),
                          SSL_get_cipher_name(ssl));
            log(line);
            startSession(now);
        }
        else
        {
            const int e = SSL_get_error(ssl, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            {
                const std::string why = m_pinChecked && !m_pinOk
                                            ? "WebAdmin's certificate does not match adminCertSha256 (peer SPKI sha256 " +
                                                  lastPeerPin() + ")"
                                            : "TLS handshake failed: " + sslErrors();
                // Let the alert reach the peer before closing.
                while (BIO_ctrl_pending(static_cast<BIO*>(m_wbio)) > 0)
                {
                    char out[4096];
                    const int n = BIO_read(static_cast<BIO*>(m_wbio), out, sizeof(out));
                    if (n <= 0)
                        break;
                    m_netOut.insert(m_netOut.end(), out, out + n);
                }
                writeSocket();
                closeLink(why);
                return;
            }
        }
    }

    if (m_state == Open)
    {
        while (true)
        {
            ERR_clear_error();
            const int n = SSL_read(ssl, &m_buffer[0], static_cast<int>(m_buffer.size()));
            if (n > 0)
            {
                if (m_session && !m_session->onBytes(&m_buffer[0], static_cast<size_t>(n), m_appOut))
                {
                    const std::string why = m_session->closeReason();
                    // Encrypt the final Error, then close.
                    if (m_appPos < m_appOut.size())
                        SSL_write(ssl, &m_appOut[m_appPos], static_cast<int>(m_appOut.size() - m_appPos));
                    m_appOut.clear();
                    m_appPos = 0;
                    while (BIO_ctrl_pending(static_cast<BIO*>(m_wbio)) > 0)
                    {
                        char out[4096];
                        const int k = BIO_read(static_cast<BIO*>(m_wbio), out, sizeof(out));
                        if (k <= 0)
                            break;
                        m_netOut.insert(m_netOut.end(), out, out + k);
                    }
                    writeSocket();
                    closeLink(why);
                    return;
                }
                noteAuthentication();
                continue;
            }
            const int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                break;
            closeLink(e == SSL_ERROR_ZERO_RETURN ? std::string("WebAdmin closed the TLS session")
                                                 : "TLS read failed: " + sslErrors());
            return;
        }
        while (m_appPos < m_appOut.size() && m_netOut.size() - m_netPos < kRefillBelow)
        {
            size_t chunk = m_appOut.size() - m_appPos;
            if (chunk > kTlsWriteChunk)
                chunk = kTlsWriteChunk;
            ERR_clear_error();
            const int n = SSL_write(ssl, &m_appOut[m_appPos], static_cast<int>(chunk));
            if (n > 0)
            {
                m_appPos += static_cast<size_t>(n);
                // Drain each record so the memory BIO stays small.
                while (BIO_ctrl_pending(static_cast<BIO*>(m_wbio)) > 0)
                {
                    char out[16 * 1024 + 512];
                    const int k = BIO_read(static_cast<BIO*>(m_wbio), out, sizeof(out));
                    if (k <= 0)
                        break;
                    m_netOut.insert(m_netOut.end(), out, out + k);
                }
                continue;
            }
            const int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                break;
            closeLink("TLS write failed: " + sslErrors());
            return;
        }
        if (m_appPos == m_appOut.size())
        {
            m_appOut.clear();
            m_appPos = 0;
        }
    }

    if (m_wbio)
    {
        while (BIO_ctrl_pending(static_cast<BIO*>(m_wbio)) > 0)
        {
            char out[4096];
            const int n = BIO_read(static_cast<BIO*>(m_wbio), out, sizeof(out));
            if (n <= 0)
                break;
            m_netOut.insert(m_netOut.end(), out, out + n);
        }
    }
}

void AdminIo::writeSocket()
{
    while (m_fd >= 0 && m_state != Connecting && m_netPos < m_netOut.size())
    {
        const ssize_t sent = ::send(m_fd, &m_netOut[m_netPos], m_netOut.size() - m_netPos, MSG_NOSIGNAL);
        if (sent > 0)
        {
            m_netPos += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        closeLink(errnoText("send", errno));
        return;
    }
    if (m_netPos == m_netOut.size())
    {
        m_netOut.clear();
        m_netPos = 0;
    }
}

void AdminIo::noteAuthentication()
{
    if (m_counted || !m_session || !m_session->authenticated())
        return;
    m_counted = true;
    ++m_authenticatedLinks;
    char line[160];
    std::snprintf(line, sizeof(line), "admin: link session %u to %s:%d authenticated (server id '%s')", m_sessionCounter,
                  m_config.host.c_str(), m_config.port, m_config.serverId.c_str());
    log(line);
}

void AdminIo::sendPing(int64_t now)
{
    std::vector<uint8_t> f;
    encodePing(f, MsgPing, ++m_pingId, static_cast<uint64_t>(now));
    m_appOut.insert(m_appOut.end(), f.begin(), f.end());
    m_pingOutstanding = true;
}

void AdminIo::closeLink(const std::string& reason)
{
    const bool wasAuthenticated = m_session && m_session->authenticated();
    if (m_ssl)
        SSL_free(static_cast<SSL*>(m_ssl));   // frees both BIOs
    m_ssl = m_rbio = m_wbio = 0;
    if (m_fd >= 0)
    {
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
    }
    m_fd = -1;
    char line[160];
    const uint64_t frames = m_session ? m_session->frames() : 0;
    const uint32_t session = m_session ? m_sessionCounter : 0;
    delete m_session;
    m_session = 0;
    m_appOut.clear();
    m_appPos = 0;
    m_netOut.clear();
    m_netPos = 0;
    m_state = Idle;
    if (wasAuthenticated)
    {
        m_link->closeSession();
        Command down;
        down.type = Command::LinkDown;
        down.session = session;
        m_link->pushCommand(down);
        m_failures = 0;
    }
    else
        ++m_failures;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_lastClose = reason;
    }

    // 1 s after a link that authenticated, else doubled per failure up to the maximum, +-20% jitter.
    int64_t delay = m_config.initialBackoffMs;
    for (int i = 1; i < m_failures && delay < m_config.maxBackoffMs; ++i)
        delay *= 2;
    if (delay > m_config.maxBackoffMs)
        delay = m_config.maxBackoffMs;
    static std::mt19937 rng(static_cast<unsigned>(nowMs()));
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    delay = static_cast<int64_t>(delay * jitter(rng));
    m_nextAttempt = nowMs() + delay;
    std::snprintf(line, sizeof(line), "admin: link %s:%d closed after %llu frames%s; reconnecting in %.1f s: ",
                  m_config.host.c_str(), m_config.port, static_cast<unsigned long long>(frames),
                  wasAuthenticated ? " (authenticated)" : "", delay / 1000.0);
    log(line + reason);
}

} // namespace admin
} // namespace serverext
