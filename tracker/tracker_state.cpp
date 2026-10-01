#include "tracker_state.h"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>

#include "../common/sha1.h"

void TrackerState::set_tracker_no(int n) {
    std::unique_lock lock(mutex_);
    tracker_no_ = n;
}

void TrackerState::mark_remote_users_offline() {
    std::unique_lock lock(mutex_);
    for (auto& [id, u] : users_) {
        if (u.remote) u.online = false;
    }
}

std::vector<std::vector<std::string>> TrackerState::snapshot_ops() const {
    std::shared_lock lock(mutex_);
    std::vector<std::vector<std::string>> ops;

    for (const auto& [id, u] : users_) {
        ops.push_back({"create_user", id, u.pass_hash, std::to_string(u.origin)});
    }
    // Only sessions that live HERE: the peer is authoritative for its own.
    for (const auto& [id, u] : users_) {
        if (u.online && !u.remote) {
            ops.push_back({"login", id, u.listen_ip, std::to_string(u.listen_port)});
        }
    }
    for (const auto& [gid, g] : groups_) {
        ops.push_back({"create_group", gid, g.owner});
        for (const auto& m : g.members) {
            if (m != g.owner) ops.push_back({"accept_request", gid, m});
        }
        for (const auto& p : g.pending_requests) {
            ops.push_back({"request_join", gid, p});
        }
    }
    for (const auto& [gid, group_files] : files_) {
        for (const auto& [name, meta] : group_files) {
            // Uploader is left empty: the real seeders follow as add_seeder ops.
            std::vector<std::string> op = {"upload_file", gid, "", name,
                                            std::to_string(meta.size), meta.full_hash};
            op.insert(op.end(), meta.piece_hashes.begin(), meta.piece_hashes.end());
            ops.push_back(op);
            for (const auto& s : meta.seeders) {
                ops.push_back({"add_seeder", gid, s, name});
            }
        }
    }
    return ops;
}

void TrackerState::set_replication_sink(ReplicationSink sink) {
    std::unique_lock lock(mutex_);
    sink_ = std::move(sink);
}

void TrackerState::emit(const std::vector<std::string>& fields) {
    if (sink_) sink_(fields);
}

void TrackerState::drop_seeder_from_group_locked(const std::string& group_id, const std::string& user_id) {
    auto fit = files_.find(group_id); // find, not operator[]: don't create an empty map
    if (fit == files_.end()) return;
    for (auto& [name, meta] : fit->second) {
        meta.seeders.erase(user_id);
    }
}

namespace {

constexpr int kPasswordRounds = 5000;

// 16 random bytes from the OS, hex-encoded (32 chars).
std::string random_salt_hex() {
    unsigned char buf[16];
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(buf), sizeof(buf));
    if (urandom.gcount() != static_cast<std::streamsize>(sizeof(buf))) {
        std::random_device rd;                    // fallback if /dev/urandom is unavailable
        for (auto& b : buf) b = static_cast<unsigned char>(rd() & 0xff);
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buf) { out += digits[b >> 4]; out += digits[b & 0x0f]; }
    return out;
}

// SHA1 iterated over (salt + password). Feeding the previous digest back in each
// round makes every round depend on the last, so it cannot be parallelised away.
std::string stretch(const std::string& salt_hex, const std::string& password) {
    std::string h = SHA1::hash_hex(salt_hex + password);
    for (int i = 0; i < kPasswordRounds; ++i) h = SHA1::hash_hex(h + salt_hex + password);
    return h;
}

// Compares without stopping at the first differing byte, so the time taken does
// not reveal how much of a guess was right.
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

std::string TrackerState::make_password_record(const std::string& password) {
    std::string salt = random_salt_hex();
    return salt + "$" + stretch(salt, password);
}

bool TrackerState::verify_password(const std::string& record, const std::string& password) {
    size_t sep = record.find('$');
    if (sep == std::string::npos) return false;
    std::string salt = record.substr(0, sep);
    return constant_time_equal(record.substr(sep + 1), stretch(salt, password));
}

StatusCode TrackerState::create_user(const std::string& user_id, const std::string& password) {
    // Hash BEFORE taking the lock: 5000 SHA1 rounds take a few milliseconds and
    // must not block every other client while they run.
    std::string record = make_password_record(password);

    std::unique_lock lock(mutex_);
    if (users_.count(user_id)) {
        return STATUS_ERR_ALREADY_EXISTS;
    }
    User u;
    u.id = user_id;
    u.pass_hash = record;
    u.online = false;
    u.origin = tracker_no_;
    users_[user_id] = u;
    // Replicate the salted hash record, never the plaintext password.
    emit({"create_user", user_id, u.pass_hash, std::to_string(u.origin)});
    return STATUS_OK;
}

StatusCode TrackerState::login(const std::string& user_id, const std::string& password,
                      const std::string& listen_ip, int listen_port, uint64_t* session_out) {
    // Copy the stored record under a shared lock and verify OUTSIDE any lock
    // (the check is deliberately slow). For an unknown user we still run a
    // full verification against a dummy record, so the response time does not
    // reveal which usernames exist.
    static const std::string kDummyRecord = make_password_record("dummy-password-for-timing");
    std::string record;
    bool known = false;
    {
        std::shared_lock lock(mutex_);
        auto it = users_.find(user_id);
        if (it != users_.end()) {
            record = it->second.pass_hash;
            known = true;
        }
    }
    bool password_ok = verify_password(known ? record : kDummyRecord, password);
    if (!known || !password_ok) {
        return STATUS_ERR_AUTH_FAILED; // same answer whether the user or the password was wrong
    }

    std::unique_lock lock(mutex_);
    auto it = users_.find(user_id);
    if (it == users_.end()) {
        return STATUS_ERR_AUTH_FAILED;
    }
    it->second.online = true;
    it->second.remote = false; // this session is connected to US
    it->second.session = ++next_session_;
    if (session_out) *session_out = it->second.session;
    it->second.listen_ip = listen_ip;
    it->second.listen_port = listen_port;
    emit({"login", user_id, listen_ip, std::to_string(listen_port)});
    return STATUS_OK;
}

StatusCode TrackerState::logout(const std::string& user_id, uint64_t session) {
    std::unique_lock lock(mutex_);
    auto it = users_.find(user_id);
    if (it == users_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (session != 0 && it->second.session != session) {
        return STATUS_OK; // superseded by a newer login: leave that session alone
    }
    it->second.online = false;
    emit({"logout", user_id});
    return STATUS_OK;
}

bool TrackerState::user_exists(const std::string& user_id) const {
    std::shared_lock lock(mutex_);
    return users_.count(user_id) > 0;
}

StatusCode TrackerState::create_group(const std::string& group_id, const std::string& user_id) {
    std::unique_lock lock(mutex_);
    if (!users_.count(user_id)) {
        return STATUS_ERR_NOT_FOUND; // owner must be a known user
    }
    if (groups_.count(group_id)) {
        return STATUS_ERR_ALREADY_EXISTS;
    }
    Group g;
    g.id = group_id;
    g.owner = user_id;
    g.members.insert(user_id);
    groups_[group_id] = g;
    emit({"create_group", group_id, user_id});
    return STATUS_OK;
}

StatusCode TrackerState::request_join(const std::string& group_id, const std::string& user_id) {
    std::unique_lock lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    Group& g = it->second;
    if (g.members.count(user_id)) {
        return STATUS_ERR_ALREADY_MEMBER;
    }
    if (g.pending_requests.count(user_id)) {
        return STATUS_ERR_ALREADY_MEMBER; // treat duplicate request the same way
    }
    g.pending_requests.insert(user_id);
    emit({"request_join", group_id, user_id});
    return STATUS_OK;
}

StatusCode TrackerState::accept_request(const std::string& group_id, const std::string& requester_id,
                                         const std::string& user_id) {
    std::unique_lock lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    Group& g = it->second;
    if (g.owner != requester_id) {
        return STATUS_ERR_NOT_OWNER;
    }
    if (!g.pending_requests.count(user_id)) {
        return STATUS_ERR_NOT_FOUND;
    }
    g.pending_requests.erase(user_id);
    g.members.insert(user_id);
    emit({"accept_request", group_id, user_id});
    return STATUS_OK;
}

StatusCode TrackerState::leave_group(const std::string& group_id, const std::string& user_id) {
    std::unique_lock lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    Group& g = it->second;
    if (!g.members.count(user_id)) {
        return STATUS_ERR_NOT_MEMBER;
    }
    if (g.owner == user_id) {
        return STATUS_ERR_BAD_REQUEST; // owner leaving own group is not supported
    }
    g.members.erase(user_id);

    // A user who leaves must stop being advertised as a seeder for this
    // group's files, otherwise remaining members would keep being handed
    // their address. (find, not operator[], so we don't create an empty map.)
    drop_seeder_from_group_locked(group_id, user_id);
    emit({"leave_group", group_id, user_id});
    return STATUS_OK;
}

std::vector<std::string> TrackerState::list_groups() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(groups_.size());
    for (const auto& [id, g] : groups_) {
        ids.push_back(id);
    }
    return ids;
}

StatusCode TrackerState::list_pending_requests(const std::string& group_id, const std::string& requester_id,
                                                std::vector<std::string>* out) const {
    std::shared_lock lock(mutex_);
    out->clear();
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    const Group& g = it->second;
    if (g.owner != requester_id) {
        return STATUS_ERR_NOT_OWNER;
    }
    for (const auto& u : g.pending_requests) {
        out->push_back(u);
    }
    return STATUS_OK;
}

bool TrackerState::group_exists(const std::string& group_id) const {
    std::shared_lock lock(mutex_);
    return groups_.count(group_id) > 0;
}

bool TrackerState::is_member(const std::string& group_id, const std::string& user_id) const {
    std::shared_lock lock(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;
    return it->second.members.count(user_id) > 0;
}

StatusCode TrackerState::upload_file(const std::string& group_id, const std::string& uploader_id,
                                      const std::string& filename, uint64_t size,
                                      const std::string& full_hash,
                                      const std::vector<std::string>& piece_hashes) {
    std::unique_lock lock(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (!git->second.members.count(uploader_id)) {
        return STATUS_ERR_NOT_MEMBER;
    }

    auto& group_files = files_[group_id]; // creates the inner map if absent
    auto existing = group_files.find(filename);
    if (existing != group_files.end()) {
        // The name is taken. That is only acceptable as a RE-SHARE: identical
        // content (same size and hashes) uploaded by someone who is not
        // currently a seeder of it, e.g. the uploader coming back after
        // `stop share` or `logout` withdrew the file (its metadata is kept
        // precisely for this). A different file under the same name, or a
        // duplicate upload by a current seeder, is still rejected.
        FileMeta& m = existing->second;
        bool same_content = m.size == size && m.full_hash == full_hash && m.piece_hashes == piece_hashes;
        if (!same_content || m.seeders.count(uploader_id)) {
            return STATUS_ERR_ALREADY_EXISTS;
        }
        m.seeders.insert(uploader_id);
        std::vector<std::string> reshare = {"upload_file", group_id, uploader_id, filename,
                                            std::to_string(size), full_hash};
        reshare.insert(reshare.end(), piece_hashes.begin(), piece_hashes.end());
        emit(reshare);   // the peer treats a replayed upload of a known file as "add this seeder"
        return STATUS_OK;
    }

    FileMeta meta;
    meta.name = filename;
    meta.size = size;
    meta.full_hash = full_hash;
    meta.piece_hashes = piece_hashes;
    meta.seeders.insert(uploader_id);
    group_files[filename] = meta;

    std::vector<std::string> op = {"upload_file", group_id, uploader_id, filename,
                                    std::to_string(size), full_hash};
    op.insert(op.end(), piece_hashes.begin(), piece_hashes.end());
    emit(op);
    return STATUS_OK;
}

StatusCode TrackerState::list_files(const std::string& group_id, const std::string& requester_id,
                                     std::vector<std::string>* out) const {
    std::shared_lock lock(mutex_);
    out->clear();
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (!git->second.members.count(requester_id)) {
        return STATUS_ERR_NOT_MEMBER;
    }
    auto fit = files_.find(group_id);
    if (fit == files_.end()) {
        return STATUS_OK; // group exists, has no files yet -- empty list, not an error
    }
    for (const auto& [name, meta] : fit->second) {
        out->push_back(name);
    }
    return STATUS_OK;
}

StatusCode TrackerState::get_file_info(const std::string& group_id, const std::string& requester_id,
                                        const std::string& filename, FileMeta* out_meta,
                                        std::vector<PeerAddr>* out_seeder_addrs) const {
    std::shared_lock lock(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (!git->second.members.count(requester_id)) {
        return STATUS_ERR_NOT_MEMBER;
    }
    auto fit = files_.find(group_id);
    if (fit == files_.end() || !fit->second.count(filename)) {
        return STATUS_ERR_NOT_FOUND;
    }

    const FileMeta& meta = fit->second.at(filename);
    *out_meta = meta;

    out_seeder_addrs->clear();
    for (const auto& seeder_id : meta.seeders) {
        // Never hand a client its own address: now that downloaders become
        // seeders, a re-download would otherwise connect to itself.
        if (seeder_id == requester_id) continue;
        auto uit = users_.find(seeder_id);
        if (uit != users_.end() && uit->second.online && !uit->second.listen_ip.empty()) {
            out_seeder_addrs->push_back({uit->second.listen_ip, uit->second.listen_port});
        }
        // Offline seeders (or ones that never logged in with an address)
        // are silently skipped -- they simply can't be downloaded from
        // right now, but their piece hashes are still valid.
    }
    return STATUS_OK;
}

StatusCode TrackerState::stop_share(const std::string& group_id, const std::string& user_id,
                                     const std::string& filename) {
    std::unique_lock lock(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    auto fit = files_.find(group_id);
    if (fit == files_.end() || !fit->second.count(filename)) {
        return STATUS_ERR_NOT_FOUND;
    }
    FileMeta& meta = fit->second.at(filename);
    if (!meta.seeders.count(user_id)) {
        return STATUS_ERR_NOT_MEMBER; // reusing this code to mean "wasn't sharing it"
    }
    meta.seeders.erase(user_id);
    emit({"stop_share", group_id, user_id, filename});
    return STATUS_OK;
}

StatusCode TrackerState::add_seeder(const std::string& group_id, const std::string& user_id,
                                     const std::string& filename) {
    std::unique_lock lock(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    if (!git->second.members.count(user_id)) {
        return STATUS_ERR_NOT_MEMBER;
    }
    auto fit = files_.find(group_id);
    if (fit == files_.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    auto mit = fit->second.find(filename);
    if (mit == fit->second.end()) {
        return STATUS_ERR_NOT_FOUND;
    }
    mit->second.seeders.insert(user_id); // std::set: re-inserting is a no-op
    emit({"add_seeder", group_id, user_id, filename});
    return STATUS_OK;
}

// ---------------------------------------------------------------------------
// Applying operations received from the peer tracker.
// ---------------------------------------------------------------------------
bool TrackerState::apply_replicated_op(const std::vector<std::string>& f) {
    if (f.empty()) return false;
    const std::string& op = f[0];
    std::unique_lock lock(mutex_);

    if (op == "create_user" && f.size() == 4) {
        int origin = 0;
        try { origin = std::stoi(f[3]); } catch (...) { return false; }
        auto it = users_.find(f[1]);
        if (it == users_.end()) {
            User u;
            u.id = f[1];
            u.pass_hash = f[2]; // already hashed by the origin tracker
            u.origin = origin;
            users_[f[1]] = u;
        } else if (it->second.pass_hash != f[2] && origin < it->second.origin) {
            // Same username created independently on both trackers with
            // different passwords (e.g. during a partition). Deterministic
            // tie-break: the account from the LOWER-numbered tracker wins on
            // both sides, so the trackers converge instead of diverging.
            it->second.pass_hash = f[2];
            it->second.origin = origin;
        }
        return true;
    }
    if (op == "login" && f.size() == 4) {
        auto it = users_.find(f[1]);
        if (it == users_.end()) return true; // unknown user: nothing to update
        int port = 0;
        try { port = std::stoi(f[3]); } catch (...) { return false; }
        it->second.online = true;
        it->second.remote = true; // session lives on the peer tracker
        it->second.session = 0;   // any local connection for this user is now superseded
        it->second.listen_ip = f[2];
        it->second.listen_port = port;
        return true;
    }
    if (op == "logout" && f.size() == 2) {
        auto it = users_.find(f[1]);
        // The peer can only end sessions the PEER hosts. If the user has since
        // logged in here (e.g. the client failed over to us), a late logout
        // from the old tracker must not knock the new session offline.
        if (it != users_.end() && it->second.remote) it->second.online = false;
        return true;
    }
    if (op == "create_group" && f.size() == 3) {
        if (!groups_.count(f[1])) {
            Group g;
            g.id = f[1];
            g.owner = f[2];
            g.members.insert(f[2]);
            groups_[f[1]] = g;
        }
        return true;
    }
    if (op == "request_join" && f.size() == 3) {
        auto it = groups_.find(f[1]);
        if (it != groups_.end() && !it->second.members.count(f[2])) {
            it->second.pending_requests.insert(f[2]);
        }
        return true;
    }
    if (op == "accept_request" && f.size() == 3) {
        auto it = groups_.find(f[1]);
        if (it != groups_.end()) {
            it->second.pending_requests.erase(f[2]);
            it->second.members.insert(f[2]);
        }
        return true;
    }
    if (op == "leave_group" && f.size() == 3) {
        auto it = groups_.find(f[1]);
        if (it != groups_.end()) {
            it->second.members.erase(f[2]);
            it->second.pending_requests.erase(f[2]);
            drop_seeder_from_group_locked(f[1], f[2]);
        }
        return true;
    }
    if (op == "upload_file" && f.size() >= 6) {
        if (!groups_.count(f[1])) return true; // group unknown here: skip
        uint64_t size = 0;
        try { size = std::stoull(f[4]); } catch (...) { return false; }
        auto& group_files = files_[f[1]];
        auto fit = group_files.find(f[3]);
        if (fit == group_files.end()) {
            FileMeta meta;
            meta.name = f[3];
            meta.size = size;
            meta.full_hash = f[5];
            meta.piece_hashes.assign(f.begin() + 6, f.end());
            if (!f[2].empty()) meta.seeders.insert(f[2]); // empty in snapshots
            group_files[f[3]] = meta;
        } else if (!f[2].empty()) {
            fit->second.seeders.insert(f[2]); // replay: just make sure uploader is a seeder
        }
        return true;
    }
    if ((op == "stop_share" || op == "add_seeder") && f.size() == 4) {
        auto git = files_.find(f[1]);
        if (git == files_.end()) return true;
        auto mit = git->second.find(f[3]);
        if (mit == git->second.end()) return true;
        if (op == "stop_share") mit->second.seeders.erase(f[2]);
        else mit->second.seeders.insert(f[2]);
        return true;
    }
    return false; // unknown op or wrong field count
}
