#ifndef TRACKER_REPLICATOR_H
#define TRACKER_REPLICATOR_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/tracker_info.h"
#include "../common/protocol.h"

// Streams this tracker's state changes to the peer tracker and keeps the
// two trackers convergent across crashes, restarts and partitions.
//
// One background sender thread owns a single persistent outbound TCP
// connection to the peer and runs this loop:
//
//   DISCONNECTED: try to connect every second.
//       on failure (first time per outage): call on_link_down() so the
//           tracker marks peer-hosted sessions offline, and drop queued ops
//           (the snapshot below will cover them);
//       on success: RESYNC -- push a full snapshot of our state (a stream of
//           idempotent ops the peer merges additively), then go CONNECTED.
//   CONNECTED: send queued live ops in order (each is ACKed). If nothing is
//       queued for a second, send a heartbeat so a dead peer is noticed
//       even when the system is idle. Any send/ACK failure -> DISCONNECTED.
//
// Each tracker runs this same loop toward the other, so a snapshot flows in
// both directions: a restarted (empty) tracker is refilled by its peer, and
// after a partition both sides absorb what the other did.
//
// Lock discipline: TrackerState -> queue mutex is the only nesting (enqueue
// runs under the state lock). The sender thread never holds the queue mutex
// while calling into TrackerState, so there is no lock-order cycle.
class Replicator {
public:
    using SnapshotFn = std::function<std::vector<std::vector<std::string>>()>;
    using LinkDownFn = std::function<void()>;

    Replicator(const TrackerAddr& peer, SnapshotFn snapshot, LinkDownFn on_link_down);
    ~Replicator(); // calls stop()

    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    void start();
    // Stops the sender thread and closes the connection. Idempotent.
    void stop();

    // Queues one live operation for the peer. Non-blocking apart from a brief
    // queue-mutex hold. If the queue is full the op is dropped BUT the overflow
    // is remembered, and the sender thread repairs the gap with a full-state
    // resync (see run()), so a slow link can never leave the trackers silently
    // different. Ignored while stopping.
    void enqueue(const std::vector<std::string>& fields);

private:
    void run();
    bool connect_to_peer();
    bool push_snapshot();
    bool send_and_wait_ack(MsgType type, const std::string& payload);
    bool is_stopping();
    // Sleeps up to `ms` milliseconds; returns true if we are stopping.
    bool sleep_or_stop(int ms);
    void close_link();

    static constexpr size_t kMaxQueue = 10000;
    static constexpr int kRetryMs = 1000;      // reconnect / heartbeat period

    TrackerAddr peer_;
    SnapshotFn snapshot_;
    LinkDownFn on_link_down_;

    int fd_ = -1;                    // touched only by the sender thread
    uint32_t next_seq_ = 1;          // touched only by the sender thread
    bool down_logged_ = false;       // sender thread only: outage already reported

    std::mutex mutex_;               // guards queue_ and stopping_
    std::condition_variable cv_;
    std::deque<std::string> queue_;  // already-serialized payloads
    bool overflowed_ = false;        // ops were dropped: a full resync is owed
    bool stopping_ = false;
    std::thread thread_;
};

#endif // TRACKER_REPLICATOR_H
