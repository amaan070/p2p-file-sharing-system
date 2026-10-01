#ifndef CLIENT_TRACKER_CLIENT_H
#define CLIENT_TRACKER_CLIENT_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../common/tracker_info.h"
#include "../common/protocol.h"
#include "file_registry.h"

struct DownloadInfo; // defined in downloader.h

// Manages the client's connection to the tracker system and sends each user
// command as a request, printing a human-readable result.
//
// Failover: if the tracker we are talking to dies (or stops answering), the
// client switches to the other tracker, transparently logs the user back in
// there, and retries the request that failed. A background monitor thread
// notices a dead tracker even while the client sits idle at the prompt, so
// the user is re-registered (and visible as a seeder) without having to type
// anything. Because the trackers replicate all state to each other, the
// other tracker already knows the user's account, groups and files.
class TrackerClient {
public:
    TrackerClient() = default;
    ~TrackerClient(); // stops the monitor thread and closes the tracker socket
    TrackerClient(const TrackerClient&) = delete;
    TrackerClient& operator=(const TrackerClient&) = delete;

    // Connects to the first reachable tracker (tries them in file order).
    // Returns true if connected to either.
    bool connect_to_trackers(const std::vector<TrackerAddr>& trackers);

    // Starts the background thread that watches the tracker connection and
    // fails over / re-logs-in on its own. Call after connect_to_trackers().
    void start_monitor();

    // Gives this client a FileRegistry to update when uploads succeed, so
    // the seeder can later serve those files to other peers.
    void set_file_registry(FileRegistry* registry);

    void create_user(const std::vector<std::string>& args);
    void login(const std::vector<std::string>& args, const std::string& own_listen_ip, int own_listen_port);
    void logout();
    void create_group(const std::vector<std::string>& args);
    void join_group(const std::vector<std::string>& args);
    void leave_group(const std::vector<std::string>& args);
    void list_groups();
    void list_requests(const std::vector<std::string>& args);
    void accept_request(const std::vector<std::string>& args);

    void upload_file(const std::vector<std::string>& args);
    void list_files(const std::vector<std::string>& args);
    // Asks the tracker for everything needed to download (group_id, filename):
    // size, hashes and the current list of seeders. Returns true and fills
    // *out on success. On failure prints the reason (tracker unreachable,
    // not a member, no such file, malformed reply) and returns false. The
    // actual transfer is done by DownloadManager, on its own thread.
    bool fetch_download_info(const std::string& group_id, const std::string& filename,
                             DownloadInfo* out);
    void stop_share(const std::vector<std::string>& args);

    // True while a user is logged in on this client.
    bool logged_in();

    // Tells the tracker to stop advertising this client as a seeder of every file
    // it is sharing, and stops serving them locally. Best-effort: gives up
    // quietly after the first request that cannot reach a tracker. Used when the
    // user logs out or quits ("logout: end session and stop sharing files").
    void unshare_all();

    // Tells the tracker this client now holds the complete, verified file
    // and can serve it (MSG_ADD_SEEDER). Returns true if the tracker
    // accepted. Called by DownloadManager from a download thread after a
    // successful download; safe because every request takes conn_mutex_.
    bool announce_seeder(const std::string& group_id, const std::string& filename);

private:
    FileRegistry* registry_ = nullptr;

    // ---- Connection + session state, all guarded by conn_mutex_ ----
    // The mutex is held for a whole request/response exchange, so the main
    // thread and the monitor thread never interleave bytes on the socket.
    std::mutex conn_mutex_;
    int fd_ = -1;                        // -1 = not connected
    uint32_t next_seq_ = 1;
    std::vector<TrackerAddr> trackers_;
    int cur_ = -1;                       // index into trackers_ of the current connection
    // What we need to log back in after a failover. The password is kept in
    // memory only (never written to disk) and scrubbed on logout.
    bool logged_in_ = false;             // the user wants to be logged in
    bool session_active_ = false;        // ...and the current tracker connection has a live session
    bool relogin_warned_ = false;
    std::string user_, password_, listen_ip_;
    int listen_port_ = 0;

    // ---- Monitor thread ----
    std::thread monitor_;
    std::mutex mon_mutex_;
    std::condition_variable mon_cv_;
    bool mon_stop_ = false;
    void monitor_loop();

    struct RequestResult {
        bool ok = false;             // true if a well-formed response was received at all
        bool conn_error = false;     // true if the failure was the connection itself (no valid response)
        StatusCode status = STATUS_ERR_INTERNAL;
        std::vector<std::string> fields; // response fields after the status code
    };

    // ---- Helpers below require conn_mutex_ to be held ----
    // One request/response exchange on the current socket. No failover; on a
    // connection-level failure it closes the socket and sets conn_error.
    RequestResult exchange_locked(MsgType type, const std::vector<std::string>& fields);
    void drop_connection_locked();
    // Connects to a tracker (the OTHER one first), then restores the login.
    bool reconnect_locked();
    bool relogin_locked();

    static void print_result(const RequestResult& r);

    // Sends a request and returns the structured result without printing
    // anything, so callers that need the actual data (upload_file to know
    // whether to register with the FileRegistry, fetch_download_info to get
    // the peer/hash data) can act on it directly.
    // On a connection-level failure it fails over to the other tracker,
    // restores the login, and retries ONCE (allow_failover=false disables this).
    RequestResult send_request_ex(MsgType type, const std::vector<std::string>& fields,
                                  bool allow_failover = true);

    // Convenience wrapper for commands that just print OK/ERROR and don't
    // need the structured result.
    bool send_request(MsgType type, const std::vector<std::string>& fields);
};

#endif // CLIENT_TRACKER_CLIENT_H
