#include "downloader.h"

#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../common/file_chunker.h"
#include "../common/network_utils.h"
#include "../common/protocol.h"
#include "../common/sha1.h"
#include "../common/tracker_info.h"

// Prints one complete line with a single stream insertion. Several downloads
// now run on different threads at once; building the whole line first and
// writing it in one go keeps their lines from being spliced together
// mid-sentence on the terminal.
static void say(const std::string& line) {
    std::cout << line << std::flush;
}

bool parse_download_info(const std::vector<std::string>& fields, DownloadInfo* out) {
    try {
        size_t idx = 0;
        if (fields.size() < 3) return false;

        out->file_size = std::stoull(fields[idx++]);
        out->full_hash = fields[idx++];
        size_t num_pieces = std::stoul(fields[idx++]);

        if (fields.size() < idx + num_pieces + 1) return false;
        out->piece_hashes.clear();
        for (size_t i = 0; i < num_pieces; ++i) {
            out->piece_hashes.push_back(fields[idx++]);
        }

        size_t num_seeders = std::stoul(fields[idx++]);
        if (fields.size() < idx + num_seeders) return false;
        out->peers.clear();
        for (size_t i = 0; i < num_seeders; ++i) {
            TrackerAddr addr;
            if (!parse_ip_port(fields[idx++], &addr)) return false;
            out->peers.push_back({addr.ip, addr.port});
        }
    } catch (...) {
        return false;
    }
    return true;
}

// How long we wait on a peer before giving up on it. Without a timeout, a peer
// that freezes (or a network that silently drops packets) would block a download
// thread in recv() forever -- and since `logout` waits for downloads, hang the
// client too. The timeout applies to each individual send/recv, not to a whole
// piece, so a slow-but-alive peer is not penalised.
static std::atomic<int> g_peer_timeout_seconds{8};

void set_peer_timeout_seconds(int seconds) {
    if (seconds > 0) g_peer_timeout_seconds.store(seconds);
}

static int connect_to_peer(const DownloadPeer& peer) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval tv{g_peer_timeout_seconds.load(), 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); // also bounds connect() on Linux
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(peer.port));
    if (inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    set_tcp_nodelay(fd);
    return fd;
}

// Why a piece fetch failed. The caller treats these differently:
//   FETCH_NET_ERROR  the connection broke or timed out. Possibly transient, so
//                    the same peer may be given one more chance.
//   FETCH_BAD_DATA   the peer answered but the answer is wrong or refused
//                    (hash mismatch, wrong piece, "not found"). Retrying the
//                    same peer for this piece is pointless: it goes elsewhere.
enum FetchResult { FETCH_OK, FETCH_NET_ERROR, FETCH_BAD_DATA };

// Requests one piece from an already-connected peer and verifies it against
// `expected_hash`. On FETCH_OK fills `out_data`.
static FetchResult fetch_and_verify_piece(int peer_fd, const std::string& group_id, const std::string& filename,
                                          uint32_t piece_index, const std::string& expected_hash,
                                          uint32_t seq_no, std::vector<uint8_t>* out_data) {
    std::string req_payload = join_fields({group_id, filename, std::to_string(piece_index)});
    if (!send_msg(peer_fd, MSG_PIECE_REQUEST, req_payload, seq_no)) {
        return FETCH_NET_ERROR;
    }

    MsgType resp_type;
    std::vector<uint8_t> resp_payload;
    uint32_t resp_seq;
    if (!recv_msg(peer_fd, &resp_type, &resp_payload, &resp_seq)) {
        return FETCH_NET_ERROR;  // closed, timed out, or garbage on the wire
    }
    if (resp_type != MSG_PIECE_DATA || resp_payload.size() < 4) {
        return FETCH_BAD_DATA;   // peer refused (e.g. not sharing this file) or malformed reply
    }

    uint32_t idx_be;
    std::memcpy(&idx_be, resp_payload.data(), 4);
    if (ntohl(idx_be) != piece_index) {
        return FETCH_BAD_DATA;   // response doesn't correspond to the piece we asked for
    }

    std::vector<uint8_t> piece_data(resp_payload.begin() + 4, resp_payload.end());
    std::string piece_str(piece_data.begin(), piece_data.end());
    if (SHA1::hash_hex(piece_str) != expected_hash) {
        return FETCH_BAD_DATA;   // corrupted piece -- must be re-fetched from a DIFFERENT peer
    }

    *out_data = std::move(piece_data);
    return FETCH_OK;
}

bool download_file_single_peer(const std::string& group_id, const std::string& filename,
                                const DownloadInfo& info, const std::string& dest_path,
                                DownloadProgress* progress) {
    if (info.peers.empty()) {
        say("No seeders available for " + filename + ".\n");
        return false;
    }

    uint32_t num_pieces = static_cast<uint32_t>(info.piece_hashes.size());
    if (progress) progress->total_pieces.store(num_pieces);
    {
        std::ostringstream oss;
        oss << "Downloading " << filename << " (" << info.file_size << " bytes, "
            << num_pieces << " piece" << (num_pieces == 1 ? "" : "s") << ")...\n";
        say(oss.str());
    }

    size_t peer_idx = 0;
    int peer_fd = -1;
    while (peer_idx < info.peers.size()) {
        peer_fd = connect_to_peer(info.peers[peer_idx]);
        if (peer_fd >= 0) break;
        say("Could not connect to peer " + info.peers[peer_idx].ip + ":" +
            std::to_string(info.peers[peer_idx].port) + ", trying next...\n");
        ++peer_idx;
    }
    if (peer_fd < 0) {
        say("Could not connect to any seeder for " + filename + ".\n");
        return false;
    }

    const int max_attempts_per_piece = 3;
    uint32_t seq = 1;

    for (uint32_t i = 0; i < num_pieces; ++i) {
        std::vector<uint8_t> piece_data;
        bool got_piece = false;

        for (int attempt = 0; attempt < max_attempts_per_piece && !got_piece; ++attempt) {
            if (fetch_and_verify_piece(peer_fd, group_id, filename, i, info.piece_hashes[i], seq++, &piece_data) == FETCH_OK) {
                got_piece = true;
                break;
            }
            say("Piece " + std::to_string(i) + " of " + filename + " failed/corrupted (attempt " +
                std::to_string(attempt + 1) + "/" + std::to_string(max_attempts_per_piece) +
                "), retrying...\n");

            close(peer_fd);
            peer_fd = connect_to_peer(info.peers[peer_idx]);
            if (peer_fd < 0) {
                say("Lost connection to peer and could not reconnect.\n");
                break;
            }
        }

        if (!got_piece) {
            say("Failed to download piece " + std::to_string(i) + " of " + filename + " after retries.\n");
            if (peer_fd >= 0) close(peer_fd);
            return false;
        }

        if (!write_piece(dest_path, i, piece_data)) {
            say("Failed to write piece " + std::to_string(i) + " to " + dest_path + ".\n");
            close(peer_fd);
            return false;
        }
        if (progress) {
            progress->completed_pieces++;  // shown by `show downloads`
        } else {
            say("  Piece " + std::to_string(i + 1) + "/" + std::to_string(num_pieces) +
                " verified and written.\n");
        }
    }

    close(peer_fd);

    FileMetadata final_meta;
    if (!compute_file_metadata(dest_path, &final_meta) || final_meta.full_hash != info.full_hash) {
        say("Whole-file hash mismatch after download of " + filename + " -- file may be corrupted.\n");
        return false;
    }

    say("[C] [" + group_id + "] " + filename + "\n");
    return true;
}

namespace {

// Shared state of one multi-peer download.
//
// Analogy: a job board in a workshop. Each piece is a job card on the board.
// A worker (bound to one peer) takes a card, tries to do it, and either
// finishes it or puts it back. The important rule: the board remembers WHICH
// worker failed WHICH card, and never hands a card back to a worker who already
// failed it. That is what stops one faulty peer from sabotaging the download:
// its failures are pinned on it, and the cards go to workers who can do them.
class SharedDownloadState {
public:
    // A peer that fails a piece this many times (counting a bad answer as
    // kPeerFailLimit at once) is never asked for that piece again.
    static constexpr int kPeerFailLimit = 2;
    // A worker whose peer fails this many pieces IN A ROW gives that peer up.
    static constexpr int kMaxConsecutiveFailures = 3;

    explicit SharedDownloadState(uint32_t num_pieces) : failures_(num_pieces) {
        for (uint32_t i = 0; i < num_pieces; ++i) pending_.push_back(i);
    }

    // Blocks until there is a piece `peer_idx` may fetch and returns true with
    // it, or returns false when this worker has nothing more to do: either all
    // pieces are finished, or nobody can make progress any more.
    //
    // It must WAIT (not return) while other workers still hold pieces: one of
    // them may fail and put a piece back that only THIS worker can serve.
    bool take(size_t peer_idx, uint32_t* out_piece) {
        std::unique_lock<std::mutex> lock(mu_);
        while (true) {
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (eligible_locked(*it, peer_idx)) {
                    *out_piece = *it;
                    pending_.erase(it);
                    ++in_flight_;
                    return true;
                }
            }
            if (in_flight_ == 0) return false;  // nothing eligible and no one working
            cv_.wait(lock);                     // a failure elsewhere may change that
        }
    }

    // The worker fetched, verified and wrote the piece.
    void finish() {
        std::lock_guard<std::mutex> lock(mu_);
        --in_flight_;
        ++completed_;
        cv_.notify_all();
    }

    // The worker failed to get the piece: put it back, remembering who failed.
    // `weight` = how much of the peer's allowance this uses up.
    void fail(uint32_t piece, size_t peer_idx, int weight) {
        std::lock_guard<std::mutex> lock(mu_);
        --in_flight_;
        failures_[piece][peer_idx] += weight;
        pending_.push_back(piece);
        cv_.notify_all();
    }

    uint32_t completed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return completed_;
    }

    // Peers are claimed from the tracker's list in order; a worker whose peer is
    // dead or dropped claims the next unclaimed one.
    size_t claim_next_peer_index() { return next_peer_.fetch_add(1); }

private:
    bool eligible_locked(uint32_t piece, size_t peer_idx) const {
        auto it = failures_[piece].find(peer_idx);
        return it == failures_[piece].end() || it->second < kPeerFailLimit;
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint32_t> pending_;                 // pieces waiting for a worker
    std::vector<std::map<size_t, int>> failures_;  // failures_[piece][peer] = failure weight
    size_t in_flight_ = 0;                         // pieces currently held by workers
    uint32_t completed_ = 0;
    std::atomic<size_t> next_peer_{0};
};

// Claims not-yet-tried peers from the shared pool, in list order, until one
// accepts a connection. Returns its fd and fills *idx, or -1 when the whole list
// has been claimed (nothing left to try).
int connect_next_peer(SharedDownloadState* shared, const std::vector<DownloadPeer>& peers, size_t* idx) {
    while (true) {
        size_t i = shared->claim_next_peer_index();
        if (i >= peers.size()) return -1;
        int fd = connect_to_peer(peers[i]);
        if (fd >= 0) {
            *idx = i;
            return fd;
        }
    }
}

void download_worker(SharedDownloadState* shared, const std::vector<DownloadPeer>* peers,
                      const std::string& group_id, const std::string& filename,
                      const std::vector<std::string>* piece_hashes, const std::string* dest_path,
                      DownloadProgress* progress) {
    size_t peer_idx = 0;
    int peer_fd = connect_next_peer(shared, *peers, &peer_idx);
    if (peer_fd < 0) {
        return;  // every peer in the list was already claimed or unreachable
    }

    uint32_t seq = 1;
    int consecutive_failures = 0;
    uint32_t index;
    while (peer_fd >= 0 && shared->take(peer_idx, &index)) {
        std::vector<uint8_t> piece_data;
        FetchResult r = fetch_and_verify_piece(peer_fd, group_id, filename, index,
                                                (*piece_hashes)[index], seq++, &piece_data);

        if (r == FETCH_OK && write_piece(*dest_path, index, piece_data)) {
            shared->finish();
            consecutive_failures = 0;
            if (progress) progress->completed_pieces++;  // shown by `show downloads`
            continue;
        }

        // Failure. Hand the piece back and pin the failure on this peer. A bad
        // answer (corrupt / refused) uses up the peer's whole allowance for
        // this piece at once; a network error only a part, since a dropped
        // connection can be transient and worth one retry.
        int weight = (r == FETCH_BAD_DATA) ? SharedDownloadState::kPeerFailLimit : 1;
        shared->fail(index, peer_idx, weight);
        ++consecutive_failures;

        // The connection may be in an unknown state after a failure: replace it.
        close(peer_fd);
        peer_fd = -1;
        if (consecutive_failures >= SharedDownloadState::kMaxConsecutiveFailures) {
            // This peer keeps failing: stop using it for this download and move
            // on to the next unclaimed peer (if any).
            peer_fd = connect_next_peer(shared, *peers, &peer_idx);
            consecutive_failures = 0;
        } else {
            peer_fd = connect_to_peer((*peers)[peer_idx]);
            if (peer_fd < 0) {
                peer_fd = connect_next_peer(shared, *peers, &peer_idx);
                consecutive_failures = 0;
            }
        }
    }

    if (peer_fd >= 0) close(peer_fd);
}

} // namespace

bool download_file_multi_peer(const std::string& group_id, const std::string& filename,
                               const DownloadInfo& info, const std::string& dest_path,
                               size_t max_workers, DownloadProgress* progress) {
    if (info.peers.empty()) {
        say("No seeders available for " + filename + ".\n");
        return false;
    }

    uint32_t num_pieces = static_cast<uint32_t>(info.piece_hashes.size());

    if (info.peers.size() == 1) {
        return download_file_single_peer(group_id, filename, info, dest_path, progress);
    }

    if (progress) progress->total_pieces.store(num_pieces);
    {
        std::ostringstream oss;
        oss << "Downloading " << filename << " (" << info.file_size << " bytes, "
            << num_pieces << " pieces) from " << info.peers.size() << " peers...\n";
        say(oss.str());
    }

    if (!preallocate_file(dest_path, info.file_size)) {
        say("Failed to allocate destination file " + dest_path + ".\n");
        return false;
    }

    size_t num_workers = std::min(max_workers, info.peers.size());

    SharedDownloadState shared(num_pieces);
    std::vector<std::thread> workers;
    workers.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
        workers.emplace_back(download_worker, &shared, &info.peers, group_id, filename,
                              &info.piece_hashes, &dest_path, progress);
    }
    for (auto& t : workers) {
        t.join();
    }

    if (shared.completed() != num_pieces) {
        say("Download incomplete: " + std::to_string(shared.completed()) + "/" +
            std::to_string(num_pieces) + " pieces fetched for " + filename + ".\n");
        return false;
    }

    FileMetadata final_meta;
    if (!compute_file_metadata(dest_path, &final_meta) || final_meta.full_hash != info.full_hash) {
        say("Whole-file hash mismatch after download of " + filename + " -- file may be corrupted.\n");
        return false;
    }

    say("[C] [" + group_id + "] " + filename + "\n");
    return true;
}