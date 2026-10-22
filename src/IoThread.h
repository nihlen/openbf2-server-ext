// The bot channel's socket thread. It owns the listening and client sockets, splits the byte stream
// into frames, enforces the framing and handshake rules, and moves parsed commands into the Channel.
// It never calls into the engine or Python. Engine-free.

#ifndef SERVEREXT_IOTHREAD_H
#define SERVEREXT_IOTHREAD_H

#include <string>

#include "Channel.h"
#include "Config.h"

namespace serverext {

// Binds the listener synchronously (so a bad endpoint is reported to the caller) and then starts a
// detached thread that runs for the life of the process. channel must never be destroyed.
// @return False with error set when the socket cannot be created, bound or the thread started.
bool startIoThread(const Config& config, Channel* channel, std::string& error);

} // namespace serverext

#endif // SERVEREXT_IOTHREAD_H
