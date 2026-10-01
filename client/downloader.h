#ifndef CLIENT_DOWNLOADER_H
#define CLIENT_DOWNLOADER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct DownloadPeer {
    std::string ip;
    int port;
};

struct DownloadInfo {
    uint64_t file_size = 0;
    std::string full_hash;
    std::vector<std::string> piece_hashes;
    std::vector<DownloadPeer> peers;
};

// Live progress of one download, written by the download's worker threads and
// read by whoever wants to display it (the DownloadManager, for `show
// downloads`). Both fields are atomics so the reader never needs a lock and
// never stalls a worker. Pass a pointer to one of these to the download
// functions below; passing nullptr keeps the old behaviour (no progress
// tracking, per-piece lines printed to stdout).
struct DownloadProgress {
    std::atomic<uint32_t> total_pieces{0};
    std::atomic<uint32_t> completed_pieces{0};
};

// How long (seconds, default 8) to wait on any single send/recv/connect to a
// peer before treating it as failed. A frozen peer or a silent network drop
// would otherwise block a download thread forever. Mainly for tests, which
// need a short timeout to exercise this path quickly.
void set_peer_timeout_seconds(int seconds);

// Parses the fields returned by the tracker's MSG_DOWNLOAD_FILE response
// (everything after the status code) into a structured DownloadInfo.
// Expected field layout: size, full_hash, num_pieces, hash_0..hash_{n-1},
// num_seeders, "ip:port"_0...."ip:port"_{m-1}. Returns false if the fields
// don't match this layout.
bool parse_download_info(const std::vector<std::string>& fields, DownloadInfo* out);

// Downloads the file described by `info` into `dest_path`: connects to the
// first reachable peer, requests each piece sequentially, verifies it
// against its expected SHA1 hash immediately on receipt (retrying a failed
// or corrupted piece up to a few times, reconnecting if needed), writes
// verified pieces to disk via write_piece, and performs a final whole-file
// hash check once all pieces are written. Prints progress and, on success,
// the required "[C] [group_id] filename" completion line. Returns true on
// full success. If `progress` is non-null it is updated after every verified
// piece and the per-piece chatter is suppressed (so several downloads can
// run at once without garbling the terminal).
bool download_file_single_peer(const std::string& group_id, const std::string& filename,
                                const DownloadInfo& info, const std::string& dest_path,
                                DownloadProgress* progress = nullptr);

// Downloads the file described by `info` into `dest_path` using multiple
// peers concurrently: pre-allocates the destination file to its full size,
// then spawns up to `max_workers` threads (one per available peer, capped
// at `max_workers`), each pulling the next not-yet-fetched piece index
// from a shared work queue, requesting it from its assigned peer,
// verifying it against the expected hash, and writing it to disk. A piece
// that fails or comes back corrupted is put back on the queue for another
// worker/peer to retry, up to a global per-piece attempt cap -- this is
// what gives multi-peer download its resilience over the single-peer
// version (one bad or slow peer doesn't block pieces other peers could
// serve). Falls back to behaving like a single-peer download when only one
// peer is available. Performs the same final whole-file hash check as the
// single-peer version. Returns true only if every piece was successfully
// fetched, verified, and written. `progress` behaves as for the single-peer
// function above.
bool download_file_multi_peer(const std::string& group_id, const std::string& filename,
                               const DownloadInfo& info, const std::string& dest_path,
                               size_t max_workers = 4, DownloadProgress* progress = nullptr);

#endif // CLIENT_DOWNLOADER_H
