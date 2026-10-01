#include "tracker_client.h"

#include <arpa/inet.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "downloader.h"

#include "../common/file_chunker.h"
#include "../common/network_utils.h"

static int try_connect(const TrackerAddr& addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Timeouts so a hung tracker can't block the client forever; a timeout
    // is treated like a dead tracker and triggers failover.
    timeval snd{3, 0}, rcv{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)); // also bounds connect() on Linux
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<uint16_t>(addr.port));
    if (inet_pton(AF_INET, addr.ip.c_str(), &server.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
        close(fd);
        return -1;
    }
    set_tcp_nodelay(fd);
    return fd;
}

TrackerClient::~TrackerClient() {
    {
        std::lock_guard<std::mutex> lock(mon_mutex_);
        mon_stop_ = true;
    }
    mon_cv_.notify_all();
    if (monitor_.joinable()) monitor_.join();

    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    // Don't leave the password lying around in freed memory.
    password_.assign(password_.size(), '\0');
}

bool TrackerClient::connect_to_trackers(const std::vector<TrackerAddr>& trackers) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    trackers_ = trackers;
    for (size_t i = 0; i < trackers.size(); ++i) {
        const auto& addr = trackers[i];
        int fd = try_connect(addr);
        if (fd >= 0) {
            fd_ = fd;
            cur_ = static_cast<int>(i);
            std::cout << "Connected to tracker at " << addr.ip << ":" << addr.port << "\n";
            return true;
        }
        std::cout << "Could not connect to tracker at " << addr.ip << ":" << addr.port
                   << ", trying next...\n";
    }
    std::cerr << "Failed to connect to any tracker.\n";
    return false;
}

void TrackerClient::set_file_registry(FileRegistry* registry) {
    registry_ = registry;
}

void TrackerClient::drop_connection_locked() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    session_active_ = false;
}

TrackerClient::RequestResult TrackerClient::exchange_locked(MsgType type, const std::vector<std::string>& fields) {
    RequestResult result;
    if (fd_ < 0) {
        result.conn_error = true;
        return result;
    }

    uint32_t seq = next_seq_++;
    if (!send_msg(fd_, type, join_fields(fields), seq)) {
        drop_connection_locked();
        result.conn_error = true;
        return result;
    }

    MsgType resp_type;
    std::string resp_payload;
    uint32_t resp_seq;
    if (!recv_msg_text(fd_, &resp_type, &resp_payload, &resp_seq)) {
        drop_connection_locked(); // closed, timed out, or garbage
        result.conn_error = true;
        return result;
    }

    auto resp_fields = split_fields(resp_payload);
    int status_int = 0;
    bool parsed = !resp_fields.empty();
    if (parsed) {
        try {
            status_int = std::stoi(resp_fields[0]);
        } catch (...) {
            parsed = false;
        }
    }
    if (!parsed) {
        drop_connection_locked(); // out of sync with the tracker: start over
        result.conn_error = true;
        return result;
    }

    result.status = static_cast<StatusCode>(status_int);
    result.fields.assign(resp_fields.begin() + 1, resp_fields.end());
    result.ok = (resp_type == MSG_RESP_OK);
    return result;
}

// Tries the OTHER tracker first (the one we were on is the likely casualty),
// then the same one again. On success restores the login, if there was one.
bool TrackerClient::reconnect_locked() {
    drop_connection_locked();
    const int n = static_cast<int>(trackers_.size());
    if (n == 0) return false;
    const int old = cur_;
    const int start = (old < 0) ? 0 : (old + 1) % n;

    for (int k = 0; k < n; ++k) {
        int idx = (start + k) % n;
        int fd = try_connect(trackers_[idx]);
        if (fd < 0) continue;

        fd_ = fd;
        cur_ = idx;
        std::cout << "[failover] " << (idx != old ? "switched to" : "reconnected to")
                   << " tracker at " << trackers_[idx].ip << ":" << trackers_[idx].port << std::endl;
        if (logged_in_) relogin_locked();
        return true;
    }
    return false;
}

// Logs the remembered user back in on the current connection.
bool TrackerClient::relogin_locked() {
    RequestResult r = exchange_locked(MSG_LOGIN, {user_, password_, listen_ip_, std::to_string(listen_port_)});
    if (r.ok) {
        session_active_ = true;
        relogin_warned_ = false;
        std::cout << "[failover] session for '" << user_ << "' restored." << std::endl;
        return true;
    }
    if (!r.conn_error && !relogin_warned_) {
        // e.g. the tracker doesn't know this account (yet). The monitor keeps retrying.
        std::cout << "[failover] could not restore session for '" << user_ << "' (code "
                   << static_cast<int>(r.status) << "); will keep retrying." << std::endl;
        relogin_warned_ = true;
    }
    return false;
}

TrackerClient::RequestResult TrackerClient::send_request_ex(MsgType type, const std::vector<std::string>& fields,
                                                            bool allow_failover) {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    if (fd_ < 0 && allow_failover) reconnect_locked(); // e.g. both trackers were down earlier
    RequestResult r = exchange_locked(type, fields);
    if (r.conn_error && allow_failover) {
        // The tracker died mid-request. The trackers replicate everything, so
        // the other one can serve us; retry once. (If the first attempt did
        // reach the old tracker before it died, the retry can report e.g.
        // "already exists" -- documented limitation of at-least-once retry.)
        if (reconnect_locked()) r = exchange_locked(type, fields);
    }
    if (r.conn_error) {
        std::cout << "No tracker is reachable right now.\n";
    }
    return r;
}

void TrackerClient::print_result(const RequestResult& r) {
    std::string extra = r.fields.empty() ? "" : join_fields(r.fields);
    if (r.ok) {
        std::cout << "OK" << (extra.empty() ? "" : (": " + extra)) << "\n";
    } else {
        std::cout << "ERROR (code " << static_cast<int>(r.status) << ")"
                   << (extra.empty() ? "" : (": " + extra)) << "\n";
    }
}

bool TrackerClient::send_request(MsgType type, const std::vector<std::string>& fields) {
    RequestResult r = send_request_ex(type, fields);
    if (r.conn_error) {
        return false; // already reported
    }
    print_result(r);
    return true;
}

void TrackerClient::start_monitor() {
    monitor_ = std::thread(&TrackerClient::monitor_loop, this);
}

// Runs once a second. Takes the connection mutex, so it never overlaps a
// request in progress (it just waits for it).
void TrackerClient::monitor_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mon_mutex_);
            if (mon_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return mon_stop_; })) return;
        }

        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (fd_ < 0) {                    // earlier failover found nobody home
            reconnect_locked();
            continue;
        }

        // Peek without consuming: 0 = orderly close by the tracker, an error
        // (other than "nothing to read") = reset/broken, and unsolicited data
        // means we're out of sync. All of them mean: start over.
        char c;
        ssize_t n = recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        bool dead = (n != 0) ? (n > 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) : true;
        if (dead) {
            std::cout << "[failover] tracker connection lost." << std::endl;
            reconnect_locked();
            continue;
        }

        if (logged_in_ && !session_active_) relogin_locked(); // earlier re-login didn't take
    }
}

void TrackerClient::create_user(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "Usage: create user <user_id> <password>\n"; return; }
    send_request(MSG_CREATE_USER, args);
}

void TrackerClient::login(const std::vector<std::string>& args, const std::string& own_listen_ip, int own_listen_port) {
    if (args.size() != 2) { std::cout << "Usage: login <user_id> <password>\n"; return; }
    std::vector<std::string> fields = {args[0], args[1], own_listen_ip, std::to_string(own_listen_port)};
    RequestResult r = send_request_ex(MSG_LOGIN, fields);
    if (r.conn_error) return;
    print_result(r);
    if (r.ok) {
        // Remember who is logged in so a failover can log them back in.
        std::lock_guard<std::mutex> lock(conn_mutex_);
        logged_in_ = true;
        session_active_ = true;
        relogin_warned_ = false;
        user_ = args[0];
        password_ = args[1];
        listen_ip_ = own_listen_ip;
        listen_port_ = own_listen_port;
    }
}

void TrackerClient::logout() {
    RequestResult r;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        // No failover here: reconnecting just to log in and immediately out
        // again would be pointless. If the tracker is gone, the session is
        // already ended on its side (it cleans up on disconnect).
        r = exchange_locked(MSG_LOGOUT, {});
        logged_in_ = false;
        session_active_ = false;
        password_.assign(password_.size(), '\0');
        password_.clear();
        user_.clear();
    }
    if (r.conn_error) {
        std::cout << "Tracker unreachable; session ended locally.\n";
    } else {
        print_result(r);
    }
}

void TrackerClient::create_group(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "Usage: create group <group_id>\n"; return; }
    send_request(MSG_CREATE_GROUP, args);
}

void TrackerClient::join_group(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "Usage: join group <group_id>\n"; return; }
    send_request(MSG_JOIN_GROUP, args);
}

void TrackerClient::leave_group(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "Usage: leave group <group_id>\n"; return; }
    send_request(MSG_LEAVE_GROUP, args);
}

void TrackerClient::list_groups() {
    send_request(MSG_LIST_GROUPS, {});
}

void TrackerClient::list_requests(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "Usage: list requests <group_id>\n"; return; }
    send_request(MSG_LIST_REQUESTS, args);
}

void TrackerClient::accept_request(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "Usage: accept request <group_id> <user_id>\n"; return; }
    send_request(MSG_ACCEPT_REQUEST, args);
}

void TrackerClient::upload_file(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "Usage: upload file <group_id> <file_path>\n"; return; }
    const std::string& group_id = args[0];
    const std::string& file_path = args[1];

    FileMetadata meta;
    if (!compute_file_metadata(file_path, &meta)) {
        std::cout << "Could not read file: " << file_path << "\n";
        return;
    }

    size_t slash = file_path.find_last_of("/\\");
    std::string filename = (slash == std::string::npos) ? file_path : file_path.substr(slash + 1);

    std::vector<std::string> fields = {group_id, filename, std::to_string(meta.file_size), meta.full_hash};
    for (const auto& h : meta.piece_hashes) fields.push_back(h);

    RequestResult r = send_request_ex(MSG_UPLOAD_FILE, fields);
    if (r.ok) {
        std::cout << "OK\n";
        if (registry_) {
            registry_->register_file(group_id, filename, file_path);
        }
    } else {
        std::cout << "ERROR (code " << static_cast<int>(r.status) << ")\n";
    }
}

void TrackerClient::list_files(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "Usage: list files <group_id>\n"; return; }
    send_request(MSG_LIST_FILES, args);
}

bool TrackerClient::fetch_download_info(const std::string& group_id, const std::string& filename,
                                        DownloadInfo* out) {
    RequestResult r = send_request_ex(MSG_DOWNLOAD_FILE, {group_id, filename});
    if (r.conn_error) return false; // "No tracker is reachable" was already printed
    if (!r.ok) {
        std::cout << "ERROR (code " << static_cast<int>(r.status) << ")\n";
        return false;
    }
    if (!parse_download_info(r.fields, out)) {
        std::cout << "Received malformed download info from tracker.\n";
        return false;
    }
    return true;
}

bool TrackerClient::logged_in() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return logged_in_;
}

void TrackerClient::unshare_all() {
    if (!registry_) return;
    for (const auto& [group_id, filename] : registry_->list()) {
        RequestResult r = send_request_ex(MSG_STOP_SHARE, {group_id, filename});
        if (r.conn_error) break;                 // no tracker reachable: nothing more we can do
        registry_->unregister(group_id, filename);  // stop serving it locally as well
    }
}

bool TrackerClient::announce_seeder(const std::string& group_id, const std::string& filename) {
    RequestResult r = send_request_ex(MSG_ADD_SEEDER, {group_id, filename});
    return r.ok;
}

void TrackerClient::stop_share(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "Usage: stop share <group_id> <file_name>\n"; return; }
    RequestResult r = send_request_ex(MSG_STOP_SHARE, args);
    if (r.ok) {
        std::cout << "OK\n";
        // Stop serving it locally too; otherwise this client's seeder would
        // keep answering piece requests for a file it told the tracker it
        // no longer shares.
        if (registry_) registry_->unregister(args[0], args[1]);
    } else {
        std::cout << "ERROR (code " << static_cast<int>(r.status) << ")\n";
    }
}