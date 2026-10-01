#include "replicator.h"

#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../common/network_utils.h"

Replicator::Replicator(const TrackerAddr& peer, SnapshotFn snapshot, LinkDownFn on_link_down)
    : peer_(peer), snapshot_(std::move(snapshot)), on_link_down_(std::move(on_link_down)) {}

Replicator::~Replicator() {
    stop();
}

void Replicator::start() {
    thread_ = std::thread(&Replicator::run, this);
}

void Replicator::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    close_link();
}

bool Replicator::is_stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

bool Replicator::sleep_or_stop(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_; });
    return stopping_;
}

void Replicator::enqueue(const std::vector<std::string>& fields) {
    std::string payload = join_fields(fields);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        if (queue_.size() >= kMaxQueue) {
            overflowed_ = true;   // a snapshot will make up for everything dropped
            return;
        }
        queue_.push_back(std::move(payload));
    }
    cv_.notify_one();
}

void Replicator::close_link() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool Replicator::connect_to_peer() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Timeouts so a hung or blackholed peer can't stall the sender forever
    // (Linux applies SO_SNDTIMEO to connect() as well).
    timeval snd{2, 0}, rcv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(peer_.port));
    if (inet_pton(AF_INET, peer_.ip.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    set_tcp_nodelay(fd);
    fd_ = fd;
    return true;
}

bool Replicator::send_and_wait_ack(MsgType type, const std::string& payload) {
    uint32_t seq = next_seq_++;
    if (!send_msg(fd_, type, payload, seq)) return false;

    MsgType ack_type;
    std::string ack_payload;
    uint32_t ack_seq;
    if (!recv_msg_text(fd_, &ack_type, &ack_payload, &ack_seq)) return false;
    return ack_type == MSG_SYNC_ACK && ack_seq == seq;
}

// Pushes our whole state to the peer as idempotent ops. Live ops that were
// queued before this point are redundant (their effect is already in the
// state we are about to snapshot), so the queue is cleared first; anything
// emitted after the snapshot is captured stays queued and is sent afterwards,
// in order, so nothing is lost or reordered.
bool Replicator::push_snapshot() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        overflowed_ = false;   // the snapshot taken next covers anything dropped so far
    }
    if (!snapshot_) return true;

    std::vector<std::vector<std::string>> ops = snapshot_();
    for (const auto& op : ops) {
        if (is_stopping()) return false;
        if (!send_and_wait_ack(MSG_SYNC_OP, join_fields(op))) return false;
    }
    std::cout << "[sync] resync complete: pushed " << ops.size()
              << " state ops to peer tracker." << std::endl;
    return true;
}

void Replicator::run() {
    while (!is_stopping()) {
        // ---------------- DISCONNECTED: (re)connect + resync ----------------
        if (fd_ < 0) {
            if (!connect_to_peer()) {
                if (!down_logged_) { // once per outage, not once per retry
                    std::cout << "[sync] peer tracker " << peer_.ip << ":" << peer_.port
                              << " unreachable; retrying every second." << std::endl;
                    down_logged_ = true;
                    if (on_link_down_) on_link_down_();
                }
                {   // nothing is being sent, so don't let live ops pile up;
                    // the snapshot on reconnect covers them.
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.clear();
                    overflowed_ = false;
                }
                if (sleep_or_stop(kRetryMs)) return;
                continue;
            }
            std::cout << "[sync] connected to peer tracker " << peer_.ip << ":"
                      << peer_.port << std::endl;
            if (!push_snapshot()) {
                if (!is_stopping()) {
                    std::cout << "[sync] resync failed; will retry." << std::endl;
                }
                close_link();
                down_logged_ = true;
                if (on_link_down_) on_link_down_();
                if (sleep_or_stop(kRetryMs)) return;
                continue;
            }
            down_logged_ = false;
        }

        // ---------------- CONNECTED: live ops + heartbeat ----------------
        bool resync_owed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resync_owed = overflowed_;
        }
        if (resync_owed) {
            std::cout << "[sync] op queue overflowed; resynchronising full state." << std::endl;
            if (!push_snapshot()) {
                if (!is_stopping()) std::cout << "[sync] lost connection to peer tracker." << std::endl;
                close_link();
                down_logged_ = true;
                if (on_link_down_) on_link_down_();
            }
            continue;
        }

        std::string payload;
        bool have_op = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(kRetryMs),
                         [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            if (!queue_.empty()) {
                payload = std::move(queue_.front());
                queue_.pop_front();
                have_op = true;
            }
        }

        bool ok = have_op ? send_and_wait_ack(MSG_SYNC_OP, payload)
                          : send_and_wait_ack(MSG_SYNC_HEARTBEAT, "");
        if (!ok) {
            std::cout << "[sync] lost connection to peer tracker." << std::endl;
            close_link();
            down_logged_ = true; // we've reported this outage already
            if (on_link_down_) on_link_down_();
        }
    }
}
