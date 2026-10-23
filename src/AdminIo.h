// The admin channel's I/O thread: it dials WebAdmin (adminConnect), reconnects with backoff, runs
// TLS 1.3 with a pinned certificate (OpenSSL, non-blocking, over memory BIOs on the socket it owns)
// and the AdminSession protocol, and moves bytes between the socket and the AdminLink. It never
// calls into the engine or Python. A separate thread from the bot channel's, so a slow name lookup
// or TLS handshake never delays a sidecar. Engine-free (POSIX sockets).

#ifndef SERVEREXT_ADMINIO_H
#define SERVEREXT_ADMINIO_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AdminLink.h"

namespace serverext {
namespace admin {

struct AdminIoConfig
{
    std::string host;
    int port;
    std::string serverId;
    std::vector<uint8_t> key;
    bool tls;
    std::vector<uint8_t> pin;   // SHA-256 of the server certificate's SubjectPublicKeyInfo
    std::string build;          // Hello's build string
    int initialBackoffMs;       // first reconnect delay; doubled per failure up to maxBackoffMs
    int maxBackoffMs;
    int handshakeTimeoutMs;
    int pingAfterMs;            // send Ping after this long without receiving anything
    int silenceLimitMs;         // close after this long without receiving anything
    AdminIoConfig()
        : port(0), tls(true), initialBackoffMs(1000), maxBackoffMs(60000), handshakeTimeoutMs(10000),
          pingAfterMs(15000), silenceLimitMs(45000) {}
};

class AdminIo
{
public:
    typedef std::function<void(const std::string&)> LogFn;

    AdminIo(const AdminIoConfig& config, AdminLink* link, LogFn log);
    ~AdminIo();

    // Creates the TLS context and starts the thread. @return False with error set on failure.
    bool start(std::string& error);
    // Stops and joins the thread (tests; the module's thread runs for the life of the process).
    void stop();

    // Statistics, for tests and logs.
    int linksAuthenticated() const { return m_authenticatedLinks.load(); }
    int attempts() const { return m_attempts.load(); }
    std::string lastCloseReason() const;
    std::string lastPeerPin() const;

    // Called by OpenSSL's verify callback on the I/O thread.
    int verifyPeer(int preverify, void* storeCtx);

private:
    enum State { Idle, Connecting, Handshaking, Open };

    void run();
    void step();
    void beginConnect(int64_t now);
    void onConnected(int64_t now);
    void startSession(int64_t now);
    void readSocket(int64_t now);
    void pump(int64_t now);
    void writeSocket();
    void closeLink(const std::string& reason);
    void sendPing(int64_t now);
    void noteAuthentication();
    void log(const std::string& line) { if (m_log) m_log(line); }

    AdminIoConfig m_config;
    AdminLink* m_link;
    LogFn m_log;
    void* m_ctx;            // SSL_CTX*
    void* m_ssl;            // SSL*
    void* m_rbio;           // BIO* (network -> SSL)
    void* m_wbio;           // BIO* (SSL -> network)
    std::thread m_thread;
    std::atomic<bool> m_stop;
    std::atomic<int> m_authenticatedLinks;
    std::atomic<int> m_attempts;
    mutable std::mutex m_statsMutex;
    std::string m_lastClose;
    std::string m_peerPin;

    State m_state;
    int m_fd;
    int64_t m_nextAttempt;
    int64_t m_connectStart;
    int64_t m_lastReceive;
    int64_t m_lastDropLog;
    uint64_t m_droppedSinceLog;
    bool m_pingOutstanding;
    uint32_t m_pingId;
    int m_failures;
    bool m_pinChecked;
    bool m_pinOk;
    bool m_counted;         // this link's authentication was counted and logged
    uint32_t m_sessionCounter;
    class AdminSession* m_session;
    std::vector<uint8_t> m_appOut;   // plaintext waiting for TLS (or the socket in plain mode)
    size_t m_appPos;
    std::vector<uint8_t> m_netOut;   // bytes waiting for the socket
    size_t m_netPos;
    std::vector<uint8_t> m_buffer;
};

} // namespace admin
} // namespace serverext

#endif // SERVEREXT_ADMINIO_H
