#ifndef TRACKER_CONNECTION_HANDLER_H
#define TRACKER_CONNECTION_HANDLER_H

#include <condition_variable>
#include <mutex>
#include <set>

#include "tracker_state.h"

// Keeps track of every connection currently being served, so that shutdown
// can be clean.
//
// Why: each connection runs on a detached thread that holds a pointer to the
// tracker's TrackerState. If main() returned on `quit` while such a thread was
// still running, TrackerState would be destroyed underneath it (use-after-free).
// shutdown_and_wait() closes that gap: it stops accepting new connections,
// disconnects the existing ones and BLOCKS until every handler thread has
// left. Only then may the state be destroyed.
class ConnectionSet {
public:
    // Registers a freshly accepted connection. Returns false if shutdown has
    // begun; the caller must then close(fd) itself and not start a handler.
    bool add(int fd);

    // Called by a handler thread as its very last act touching shared state:
    // closes fd, deregisters it and wakes shutdown_and_wait().
    void done(int fd);

    // Refuses new connections, shutdown()s the live ones (unblocking handlers
    // stuck in recv) and waits until none is left. Safe to call more than once.
    void shutdown_and_wait();

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::set<int> fds_;
    bool closing_ = false;
};

// Handles one client connection for its entire lifetime: reads messages in
// a loop, dispatches each to the appropriate TrackerState method, sends
// back a response, until the client disconnects or logs out.
//
// Runs on its own thread (one per accepted connection). Closes client_fd (via
// conns->done) before returning, regardless of how the loop ends (normal
// disconnect, malformed message, error, or tracker shutdown). `fd` must have
// been registered with conns->add() first.
void handle_client_connection(int client_fd, TrackerState* state, ConnectionSet* conns);

#endif // TRACKER_CONNECTION_HANDLER_H
