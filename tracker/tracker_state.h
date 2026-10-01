#ifndef TRACKER_STATE_H
#define TRACKER_STATE_H

#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "../common/protocol.h"

struct PeerAddr {
    std::string ip;
    int port;
};

struct User {
    std::string id;
    std::string pass_hash;   // password record: "<salt_hex>$<hash_hex>" (see make_password_record)
    bool online = false;
    std::string listen_ip;   // set at login: this user's own peer-listen address
    int listen_port = 0;     // (used so other clients can connect to download from them)
    // True if this user's live session is on the PEER tracker (learned via
    // replication). Each tracker is authoritative only for the sessions
    // connected to IT: snapshots carry only local sessions, and remote
    // sessions are marked offline when the sync link drops.
    bool remote = false;
    int origin = 0;          // tracker_no that created this account (conflict tie-break)
    // Id of the current LOCAL session, bumped on every local login. Lets a
    // connection that has been superseded (client failed over / reconnected
    // and logged in again) tell that its later logout must not end the
    // user's newer session.
    uint64_t session = 0;
};

struct Group {
    std::string id;
    std::string owner;
    std::set<std::string> members;           // includes the owner
    std::set<std::string> pending_requests;  // users awaiting approval
};

struct FileMeta {
    std::string name;
    uint64_t size = 0;
    std::string full_hash;
    std::vector<std::string> piece_hashes;
    std::set<std::string> seeders; // user_ids currently sharing this file
};

// TrackerState holds all user/group data for a single tracker instance and
// protects it with a shared_mutex: reads (list operations) can happen
// concurrently, writes (create/join/accept/leave) take an exclusive lock.
//
// All public methods are self-contained (take/return simple types) and do
// their own locking internally, so callers (the connection handler threads)
// never touch the mutex directly.
class TrackerState {
public:
    // ---- Replication (tracker-to-tracker sync) ----

    // Called with the operation's fields, e.g. {"create_group", "g1", "alice"},
    // for every LOCAL mutation that succeeds. Invoked while the state lock is
    // held, so operations reach the sink in exactly the order they were
    // applied to the state (this preserves ordering between concurrent
    // client threads). The sink must therefore be quick and must never call
    // back into TrackerState. Pass an empty function to detach.
    // This tracker's number (1 or 2). Stamped on accounts it creates so a
    // same-name conflict between trackers can be resolved deterministically.
    void set_tracker_no(int n);

    // Full-state snapshot as a list of idempotent ops (users, live local
    // sessions, groups, membership, files, seeders -- in that order so every
    // op's referenced objects already exist when it is applied). The peer
    // feeds these through apply_replicated_op, i.e. an additive merge.
    std::vector<std::vector<std::string>> snapshot_ops() const;

    // Marks every user whose session lives on the peer tracker as offline.
    // Called when the sync link drops so their (possibly dead) addresses
    // stop being handed out as seeders.
    void mark_remote_users_offline();

    using ReplicationSink = std::function<void(const std::vector<std::string>&)>;
    void set_replication_sink(ReplicationSink sink);

    // Applies an operation received from the peer tracker. The origin
    // tracker already validated it, so this does NOT re-check ownership /
    // membership rules (avoids false rejections from ordering differences);
    // it only checks that referenced objects exist. Idempotent: replaying
    // an op is harmless. Never re-emits to the sink (prevents ping-pong).
    // Returns false for an unknown op or malformed fields.
    bool apply_replicated_op(const std::vector<std::string>& fields);

    // ---- User management ----

    // Registers a new user. Fails with STATUS_ERR_ALREADY_EXISTS if the
    // user_id is taken.
    StatusCode create_user(const std::string& user_id, const std::string& password);

    // Authenticates a user_id/password pair. On success, marks the user
    // online and records their peer-listen address (listen_ip/listen_port)
    // so other clients can be given it later to download files from them.
    // If session_out is non-null it receives the id of the new session; pass
    // that id back to logout() so a superseded connection can't end a newer one.
    StatusCode login(const std::string& user_id, const std::string& password, const std::string& listen_ip, int listen_port,
                     uint64_t* session_out = nullptr);

    // Marks a user offline. STATUS_ERR_NOT_FOUND if the user doesn't exist.
    // With session == 0 the logout is unconditional. With a session id it only
    // takes effect if that is still the user's current session; otherwise the
    // session was superseded by a newer login and this call is a no-op (OK).
    StatusCode logout(const std::string& user_id, uint64_t session = 0);

    bool user_exists(const std::string& user_id) const;

    // ---- Group management ----

    // Creates a group with `user_id` as owner and sole initial member.
    // STATUS_ERR_ALREADY_EXISTS if group_id is taken.
    StatusCode create_group(const std::string& group_id, const std::string& user_id);

    // Adds user_id to group_id's pending_requests. Fails if the group
    // doesn't exist, the user is already a member, or already has a
    // pending request.
    StatusCode request_join(const std::string& group_id, const std::string& user_id);

    // Owner-only: moves user_id from pending_requests to members.
    // STATUS_ERR_NOT_OWNER if requester_id isn't the group's owner.
    // STATUS_ERR_NOT_FOUND if there's no such pending request.
    StatusCode accept_request(const std::string& group_id, const std::string& requester_id,
                               const std::string& user_id);

    // Removes user_id from group_id's members. The owner cannot leave their
    // own group in this design (must be handled via group deletion /
    // ownership transfer, not supported) -- returns
    // STATUS_ERR_BAD_REQUEST if attempted.
    StatusCode leave_group(const std::string& group_id, const std::string& user_id);

    // Returns all group_ids in the system.
    std::vector<std::string> list_groups() const;

    // Returns pending_requests for group_id. Owner-only.
    // On failure (not found / not owner), `out` is left empty and the
    // corresponding StatusCode is returned.
    StatusCode list_pending_requests(const std::string& group_id, const std::string& requester_id,
                                      std::vector<std::string>* out) const;
    
    // ---- File management ----

    // Registers a file as shared by `uploader_id` within `group_id`.
    // Fails if the group doesn't exist, the uploader isn't a member, or a
    // file with this name is already shared in this group -- with one
    // exception: identical content uploaded by someone who is not currently a
    // seeder of it is accepted as a re-share (e.g. after `stop share`/`logout`).
    StatusCode upload_file(const std::string& group_id, const std::string& uploader_id,
                            const std::string& filename, uint64_t size,
                            const std::string& full_hash,
                            const std::vector<std::string>& piece_hashes);

    // Returns all filenames shared in group_id. Member-only.
    StatusCode list_files(const std::string& group_id, const std::string& requester_id,
                           std::vector<std::string>* out) const;

    // Returns full file metadata plus the listen addresses of all current
    // seeders, for a download request. Member-only.
    StatusCode get_file_info(const std::string& group_id, const std::string& requester_id,
                              const std::string& filename, FileMeta* out_meta,
                              std::vector<PeerAddr>* out_seeder_addrs) const;

    // Removes user_id from filename's seeder list within group_id. The
    // file's metadata is kept even if no seeders remain, so a returning
    // seeder (or a future re-upload) doesn't need to recompute hashes.
    StatusCode stop_share(const std::string& group_id, const std::string& user_id,
                           const std::string& filename);

    // Registers user_id as a seeder of an existing file in group_id. Called
    // after a client has downloaded AND hash-verified the complete file.
    // Member-only. Idempotent: adding a user who is already a seeder
    // returns STATUS_OK (safe for retries and, later, tracker sync replays).
    StatusCode add_seeder(const std::string& group_id, const std::string& user_id,
                           const std::string& filename);

    bool group_exists(const std::string& group_id) const;
    bool is_member(const std::string& group_id, const std::string& user_id) const;

private:
    // Sends an op to the replication sink, if one is attached. Caller must
    // hold the unique lock.
    void emit(const std::vector<std::string>& fields);
    // Removes user_id from the seeder list of every file in group_id.
    // Caller must hold the unique lock.
    void drop_seeder_from_group_locked(const std::string& group_id, const std::string& user_id);

    ReplicationSink sink_;
    int tracker_no_ = 0;
    uint64_t next_session_ = 0;
    mutable std::shared_mutex mutex_;
    std::map<std::string, User> users_;
    std::map<std::string, Group> groups_;
    std::map<std::string, std::map<std::string, FileMeta>> files_;

    // Password storage. The stored "record" has the form  <salt_hex>$<hash_hex>
    // where hash = SHA1 iterated kPasswordRounds times over (salt + password).
    // A per-user random salt means equal passwords give different records and
    // precomputed tables are useless; the iteration makes each guess of an
    // attacker thousands of times more expensive. The plaintext is never stored
    // or replicated -- only the record is. (Purpose-built schemes such as
    // bcrypt/Argon2 are stronger but are external libraries the assignment
    // does not allow; this is documented in the README.)
    static std::string make_password_record(const std::string& password);
    static bool verify_password(const std::string& record, const std::string& password);
};

#endif // TRACKER_STATE_H
