#include "connection_handler.h"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <string>

#include "../common/network_utils.h"

// Sends a simple "OK" or "ERR" response with an optional payload string.
static void send_response(int fd, StatusCode status, const std::string& payload, uint32_t seq_no) {
    MsgType type = (status == STATUS_OK) ? MSG_RESP_OK : MSG_RESP_ERR;
    std::string body = std::to_string(static_cast<int>(status));
    if (!payload.empty()) {
        body += "|" + payload;
    }
    send_msg(fd, type, body, seq_no);
}

bool ConnectionSet::add(int fd) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closing_) return false;
    fds_.insert(fd);
    return true;
}

void ConnectionSet::done(int fd) {
    // close + erase under one lock: shutdown_and_wait() only calls shutdown()
    // on fds still listed, so it can never hit an fd number that was reused.
    // notify_all() while holding the lock: once the waiter sees the set empty
    // it may let the tracker exit, so this thread must not touch shared state
    // after releasing the lock.
    std::lock_guard<std::mutex> lock(mu_);
    fds_.erase(fd);
    close(fd);
    cv_.notify_all();
}

void ConnectionSet::shutdown_and_wait() {
    std::unique_lock<std::mutex> lock(mu_);
    closing_ = true;
    for (int fd : fds_) shutdown(fd, SHUT_RDWR);   // wakes handlers blocked in recv()
    cv_.wait(lock, [this] { return fds_.empty(); });
}

void handle_client_connection(int client_fd, TrackerState* state, ConnectionSet* conns) {
    // Tracks which user_id (if any) is logged in on this connection.
    std::string session_user;
    uint64_t session_id = 0; // this connection's session (see TrackerState::login)

    while (true) {
        MsgType type;
        std::string payload;
        uint32_t seq_no;

        if (!recv_msg_text(client_fd, &type, &payload, &seq_no)) {
            break; // client disconnected or sent a malformed/garbage message
        }

        std::vector<std::string> fields = split_fields(payload);

        switch (type) {
            case MSG_CREATE_USER: {
                if (fields.size() != 2) {
                    send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->create_user(fields[0], fields[1]);
                send_response(client_fd, s, "", seq_no);
                break;
            }
                case MSG_LOGIN: {
                // fields: user_id, password, listen_ip, listen_port
                if (fields.size() != 4) {
                    send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                int listen_port = 0;
                try {
                    listen_port = std::stoi(fields[3]);
                } catch (...) {
                    send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                uint64_t new_session = 0;
                StatusCode s = state->login(fields[0], fields[1], fields[2], listen_port, &new_session);
                if (s == STATUS_OK) {
                    session_user = fields[0];
                    session_id = new_session;
                }
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_LOGOUT: {
                if (session_user.empty()) {
                    send_response(client_fd, STATUS_ERR_NOT_LOGGED_IN, "", seq_no);
                    break;
                }
                StatusCode s = state->logout(session_user, session_id);
                session_user.clear();
                session_id = 0;
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_CREATE_GROUP: {
                if (fields.size() != 1 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->create_group(fields[0], session_user);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_JOIN_GROUP: {
                if (fields.size() != 1 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->request_join(fields[0], session_user);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_LEAVE_GROUP: {
                if (fields.size() != 1 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->leave_group(fields[0], session_user);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_LIST_GROUPS: {
                auto groups = state->list_groups();
                send_response(client_fd, STATUS_OK, join_fields(groups), seq_no);
                break;
            }
            case MSG_LIST_REQUESTS: {
                if (fields.size() != 1 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                std::vector<std::string> pending;
                StatusCode s = state->list_pending_requests(fields[0], session_user, &pending);
                send_response(client_fd, s, join_fields(pending), seq_no);
                break;
            }
            case MSG_ACCEPT_REQUEST: {
                if (fields.size() != 2 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->accept_request(fields[0], session_user, fields[1]);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_UPLOAD_FILE: {
                // fields: group_id, filename, size, full_hash, piece_hash_0, piece_hash_1, ...
                if (fields.size() < 4 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                uint64_t size = 0;
                try {
                    size = std::stoull(fields[2]);
                } catch (...) {
                    send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                std::vector<std::string> piece_hashes(fields.begin() + 4, fields.end());
                StatusCode s = state->upload_file(fields[0], session_user, fields[1], size, fields[3], piece_hashes);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_LIST_FILES: {
                if (fields.size() != 1 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                std::vector<std::string> names;
                StatusCode s = state->list_files(fields[0], session_user, &names);
                send_response(client_fd, s, join_fields(names), seq_no);
                break;
            }
            case MSG_DOWNLOAD_FILE: {
                // fields: group_id, filename
                if (fields.size() != 2 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                FileMeta meta;
                std::vector<PeerAddr> seeders;
                StatusCode s = state->get_file_info(fields[0], session_user, fields[1], &meta, &seeders);
                if (s != STATUS_OK) {
                    send_response(client_fd, s, "", seq_no);
                    break;
                }
                // Response payload: size|full_hash|num_pieces|hash0|...|hashN-1|num_seeders|ip0:port0|...
                std::vector<std::string> out_fields;
                out_fields.push_back(std::to_string(meta.size));
                out_fields.push_back(meta.full_hash);
                out_fields.push_back(std::to_string(meta.piece_hashes.size()));
                for (const auto& h : meta.piece_hashes) out_fields.push_back(h);
                out_fields.push_back(std::to_string(seeders.size()));
                for (const auto& addr : seeders) out_fields.push_back(addr.ip + ":" + std::to_string(addr.port));
                send_response(client_fd, STATUS_OK, join_fields(out_fields), seq_no);
                break;
            }
            case MSG_STOP_SHARE: {
                // fields: group_id, filename
                if (fields.size() != 2 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->stop_share(fields[0], session_user, fields[1]);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            case MSG_SYNC_HEARTBEAT: {
                // Liveness probe from the peer tracker's sender thread.
                send_msg(client_fd, MSG_SYNC_ACK, "", seq_no);
                break;
            }
            case MSG_SYNC_OP: {
                // Sent by the PEER TRACKER (not a client): one replicated
                // state change. Applied without re-validation, never
                // re-forwarded. ACK carries the same seq_no so the sender
                // can match it. (Assumes a trusted network: any connection
                // can send this -- documented limitation.)
                bool ok = state->apply_replicated_op(fields);
                if (ok) {
                    send_msg(client_fd, MSG_SYNC_ACK, "", seq_no);
                } else {
                    send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                }
                break;
            }
            case MSG_ADD_SEEDER: {
                // fields: group_id, filename
                if (fields.size() != 2 || session_user.empty()) {
                    send_response(client_fd, session_user.empty() ? STATUS_ERR_NOT_LOGGED_IN : STATUS_ERR_BAD_REQUEST, "", seq_no);
                    break;
                }
                StatusCode s = state->add_seeder(fields[0], session_user, fields[1]);
                send_response(client_fd, s, "", seq_no);
                break;
            }
            default:
                send_response(client_fd, STATUS_ERR_BAD_REQUEST, "", seq_no);
                break;
        }
    }

    if (!session_user.empty()) {
        // Clean up on disconnect -- but only if this connection's session is
        // still current (a client that reconnected has a newer one).
        state->logout(session_user, session_id);
    }
    conns->done(client_fd);   // closes client_fd; must be the last use of shared state
}
