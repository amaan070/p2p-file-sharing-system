#include "seeder.h"

#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../common/file_chunker.h"
#include "../common/network_utils.h"
#include "../common/protocol.h"

namespace {

// Everything one running seeder needs, kept in one place so it can be shut
// down cleanly.
//
// Why this exists: the accept loop and the per-connection handler threads are
// detached, and they hold a raw pointer to the client's FileRegistry. If main()
// returned while one of them was mid-request, the registry would be destroyed
// under its feet (use-after-free). So we count every thread that uses this
// state, and stop_all_seeders() waits until that count reaches zero before
// anyone is allowed to free the registry.
struct SeederState {
    FileRegistry* registry = nullptr;
    int listen_fd = -1;

    std::mutex mu;                  // guards everything below
    std::condition_variable cv;     // signalled when `active` drops
    bool stopping = false;          // stop_all_seeders() has begun
    bool listen_open = false;       // listen_fd is still a valid open fd
    std::set<int> conn_fds;         // connections currently being served
    size_t active = 0;              // threads still running: accept loop + handlers
};

std::mutex g_seeders_mu;
std::vector<std::unique_ptr<SeederState>> g_seeders;

} // namespace

// Serves piece requests on one already-accepted peer connection until the
// peer disconnects. A single connection can carry many sequential
// requests, since re-connecting per piece would be wasteful for large
// files with many pieces.
static void handle_peer_connection(SeederState* st, int peer_fd) {
    FileRegistry* registry = st->registry;
    while (true) {
        MsgType type;
        std::vector<uint8_t> payload;
        uint32_t seq_no;

        if (!recv_msg(peer_fd, &type, &payload, &seq_no)) {
            break; // peer disconnected or sent a malformed message
        }

        if (type != MSG_PIECE_REQUEST) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_BAD_REQUEST)), seq_no);
            continue;
        }

        std::string text(payload.begin(), payload.end());
        std::vector<std::string> fields = split_fields(text);
        if (fields.size() != 3) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_BAD_REQUEST)), seq_no);
            continue;
        }

        const std::string& group_id = fields[0];
        const std::string& filename = fields[1];
        uint32_t piece_index;
        try {
            piece_index = static_cast<uint32_t>(std::stoul(fields[2]));
        } catch (...) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_BAD_REQUEST)), seq_no);
            continue;
        }

        std::string local_path;
        if (!registry->lookup(group_id, filename, &local_path)) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_NOT_FOUND)), seq_no);
            continue;
        }

        uint64_t file_size;
        if (!get_file_size(local_path, &file_size)) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_INTERNAL)), seq_no);
            continue;
        }

        std::vector<uint8_t> piece_data;
        if (!read_piece(local_path, piece_index, file_size, &piece_data)) {
            send_msg(peer_fd, MSG_RESP_ERR, std::to_string(static_cast<int>(STATUS_ERR_BAD_REQUEST)), seq_no);
            continue;
        }

        // Response payload: 4-byte big-endian piece_index, followed by the
        // raw piece bytes. The index is echoed back so the downloader can
        // verify which piece this response corresponds to, independent of
        // request/response ordering assumptions.
        std::vector<uint8_t> resp;
        resp.reserve(4 + piece_data.size());
        uint32_t idx_be = htonl(piece_index);
        const uint8_t* idx_bytes = reinterpret_cast<const uint8_t*>(&idx_be);
        resp.insert(resp.end(), idx_bytes, idx_bytes + 4);
        resp.insert(resp.end(), piece_data.begin(), piece_data.end());

        send_msg_bytes(peer_fd, MSG_PIECE_DATA, resp, seq_no);
    }

    // Leave the books. The fd is removed from conn_fds and closed under the
    // same lock, so stop_all_seeders() (which calls shutdown() only on fds that
    // are still listed) can never touch an fd number that has been reused.
    // notify_all() is called while still holding the lock: once the waiter
    // sees active==0 it may free `st`, so this thread must not touch `st`
    // after releasing the lock.
    std::lock_guard<std::mutex> lock(st->mu);
    st->conn_fds.erase(peer_fd);
    close(peer_fd);
    --st->active;
    st->cv.notify_all();
}

static void accept_loop(SeederState* st) {
    while (true) {
        sockaddr_in peer_addr{};
        socklen_t peer_len = sizeof(peer_addr);
        int peer_fd = accept(st->listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (peer_fd < 0) {
            if (errno == EINTR) continue;
            break; // listener shut down by stop_all_seeders(), or a fatal error
        }

        set_tcp_nodelay(peer_fd);

        bool leave = false;
        {
            std::lock_guard<std::mutex> lock(st->mu);
            if (st->stopping) {
                close(peer_fd);   // raced with stop_all_seeders(): turn this one away
                leave = true;     // decided under the lock, so no unlocked read of `stopping`
            } else {
                st->conn_fds.insert(peer_fd);
                ++st->active;  // counted BEFORE the thread exists, so stop cannot miss it
                try {
                    std::thread(handle_peer_connection, st, peer_fd).detach();
                } catch (...) {
                    // Out of threads: drop just this connection, keep listening.
                    st->conn_fds.erase(peer_fd);
                    --st->active;
                    close(peer_fd);
                }
            }
        }
        if (leave) break;
    }

    std::lock_guard<std::mutex> lock(st->mu);
    if (st->listen_open) {
        close(st->listen_fd);
        st->listen_open = false;
    }
    --st->active;
    st->cv.notify_all();
}

bool start_seeder(const std::string& ip, int port, FileRegistry* registry) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("seeder: socket");
        return false;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (ip == "0.0.0.0" || ip.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "seeder: invalid IP address: " << ip << "\n";
        close(listen_fd);
        return false;
    }

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("seeder: bind");
        close(listen_fd);
        return false;
    }

    if (listen(listen_fd, 32) < 0) {
        perror("seeder: listen");
        close(listen_fd);
        return false;
    }

    auto state = std::make_unique<SeederState>();
    state->registry = registry;
    state->listen_fd = listen_fd;
    state->listen_open = true;
    state->active = 1;  // the accept loop itself
    SeederState* raw = state.get();
    {
        std::lock_guard<std::mutex> lock(g_seeders_mu);
        g_seeders.push_back(std::move(state));
    }
    try {
        std::thread(accept_loop, raw).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(g_seeders_mu);
        close(listen_fd);
        g_seeders.pop_back();
        return false;
    }
    return true;
}

void stop_all_seeders() {
    // Take ownership of the list so a second call (or a call with nothing
    // running) simply finds it empty.
    std::vector<std::unique_ptr<SeederState>> seeders;
    {
        std::lock_guard<std::mutex> lock(g_seeders_mu);
        seeders.swap(g_seeders);
    }

    for (auto& up : seeders) {
        SeederState* st = up.get();
        std::unique_lock<std::mutex> lock(st->mu);

        // 1. Stop letting new people in, and wake the accept loop: shutdown()
        //    on a listening socket makes a blocked accept() return an error.
        st->stopping = true;
        if (st->listen_open) shutdown(st->listen_fd, SHUT_RDWR);

        // 2. Send the people already inside out: shutdown() makes a handler
        //    blocked in recv() see end-of-stream (and a mid-send fail), so it
        //    finishes and removes itself.
        for (int fd : st->conn_fds) shutdown(fd, SHUT_RDWR);

        // 3. Wait until the accept loop and every handler have left. Only after
        //    this is it safe for the caller to destroy the FileRegistry.
        st->cv.wait(lock, [st] { return st->active == 0; });
    }
    // `seeders` (and each SeederState) is freed here, after all threads left.
}
