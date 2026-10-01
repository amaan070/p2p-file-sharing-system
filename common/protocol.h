#ifndef COMMON_PROTOCOL_H
#define COMMON_PROTOCOL_H

#include <cstdint>

// ---------------------------------------------------------------------------
// Wire protocol for the P2P file sharing system.
//
// Every message on every connection (tracker<->client, client<->client,
// tracker<->tracker) starts with a fixed-size MsgHeader, followed by
// `payload_len` bytes of payload.
//
// For control messages the payload is a single '|'-delimited text string,
// e.g. "alice|password123". For piece data the payload is raw binary bytes
// (4-byte big-endian piece index followed by the piece), and payload_len
// tells the receiver exactly how many bytes to read.
// ---------------------------------------------------------------------------

// Sanity value at the start of every header, so a receiver can detect a
// desynced stream / garbage connection instead of misinterpreting bytes.
constexpr uint32_t PROTO_MAGIC = 0x50325053; // "P2PS"

// Piece size used throughout the system (bytes).
constexpr uint64_t PIECE_SIZE = 512 * 1024; // 512 KB

// Max size we allow for a single message payload (sanity bound, not a real
// file transfer limit -- piece data messages are bounded by PIECE_SIZE plus
// a small header overhead).
constexpr uint32_t MAX_PAYLOAD_SIZE = static_cast<uint32_t>(PIECE_SIZE) + 4096;

enum MsgType : uint16_t {
    // ---- Tracker <-> Client: user & group management ----
    MSG_CREATE_USER = 1,
    MSG_LOGIN = 2,
    MSG_LOGOUT = 3,
    MSG_CREATE_GROUP = 4,
    MSG_JOIN_GROUP = 5,
    MSG_LEAVE_GROUP = 6,
    MSG_LIST_GROUPS = 7,
    MSG_LIST_REQUESTS = 8,
    MSG_ACCEPT_REQUEST = 9,

    // ---- Tracker <-> Client: file management ----
    MSG_UPLOAD_FILE = 20,
    MSG_LIST_FILES = 21,
    MSG_DOWNLOAD_FILE = 22,
    MSG_STOP_SHARE = 23,
    MSG_ADD_SEEDER = 24,   // client finished a download; register as a seeder

    // ---- Client <-> Client: piece transfer ----
    MSG_PIECE_REQUEST = 41,
    MSG_PIECE_DATA = 42,

    // ---- Tracker <-> Tracker: synchronization ----
    MSG_SYNC_OP = 60,
    MSG_SYNC_ACK = 61,
    MSG_SYNC_HEARTBEAT = 62,

    // ---- Generic responses ----
    MSG_RESP_OK = 100,
    MSG_RESP_ERR = 101,
};

enum StatusCode : uint16_t {
    STATUS_OK = 0,
    STATUS_ERR_UNKNOWN = 1,
    STATUS_ERR_BAD_REQUEST = 2,       // malformed / missing fields
    STATUS_ERR_AUTH_FAILED = 3,       // bad username/password
    STATUS_ERR_NOT_LOGGED_IN = 4,     // action requires a session
    STATUS_ERR_ALREADY_EXISTS = 5,    // duplicate user/group/file
    STATUS_ERR_NOT_FOUND = 6,         // user/group/file doesn't exist
    STATUS_ERR_NOT_OWNER = 7,         // owner-only action attempted by non-owner
    STATUS_ERR_NOT_MEMBER = 8,        // action requires group membership
    STATUS_ERR_ALREADY_MEMBER = 9,    // already a member / already requested
    STATUS_ERR_INTERNAL = 10,         // server-side failure
};

#pragma pack(push, 1)
struct MsgHeader {
    uint32_t magic;        // must equal PROTO_MAGIC
    uint16_t type;         // one of MsgType
    uint32_t payload_len;  // number of bytes following this header
    uint32_t seq_no;       // used for sync ack matching / request correlation
};
#pragma pack(pop)

constexpr size_t MSG_HEADER_SIZE = sizeof(MsgHeader);

#endif // COMMON_PROTOCOL_H
