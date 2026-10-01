#include "network_utils.h"

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

void set_tcp_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

bool send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        // MSG_NOSIGNAL: writing to a socket whose peer already closed must
        // return EPIPE, not raise SIGPIPE (which would kill the whole process).
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted by signal, retry
            return false; // real error (e.g. EPIPE, ECONNRESET)
        }
        if (n == 0) return false; // shouldn't normally happen for send()
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted by signal, retry
            return false; // real error
        }
        if (n == 0) return false; // peer closed connection early
        got += static_cast<size_t>(n);
    }
    return true;
}

static bool send_msg_raw(int fd, MsgType type, const uint8_t* data, uint32_t len, uint32_t seq_no) {
    MsgHeader hdr;
    hdr.magic = PROTO_MAGIC;
    hdr.type = static_cast<uint16_t>(type);
    hdr.payload_len = len;
    hdr.seq_no = seq_no;

    // Send header + payload as ONE write. Two separate small writes interact
    // badly with TCP: Nagle's algorithm holds the second write until the first
    // is acknowledged, and the receiver delays that acknowledgement (it has
    // nothing to reply with until it gets the payload) -- a ~40 ms stall per
    // message, which capped piece transfers at ~9 MiB/s on an idle loopback.
    std::vector<uint8_t> buf(sizeof(hdr) + len);
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    if (len > 0) std::memcpy(buf.data() + sizeof(hdr), data, len);
    return send_all(fd, buf.data(), buf.size());
}

bool send_msg(int fd, MsgType type, const std::string& payload, uint32_t seq_no) {
    return send_msg_raw(fd, type, reinterpret_cast<const uint8_t*>(payload.data()),
                         static_cast<uint32_t>(payload.size()), seq_no);
}

bool send_msg_bytes(int fd, MsgType type, const std::vector<uint8_t>& payload, uint32_t seq_no) {
    return send_msg_raw(fd, type, payload.data(), static_cast<uint32_t>(payload.size()), seq_no);
}

bool recv_msg(int fd, MsgType* type, std::vector<uint8_t>* payload, uint32_t* seq_no) {
    MsgHeader hdr;
    if (!recv_all(fd, &hdr, sizeof(hdr))) return false;

    if (hdr.magic != PROTO_MAGIC) return false;           // desynced / garbage stream
    if (hdr.payload_len > MAX_PAYLOAD_SIZE) return false;  // reject absurd sizes defensively

    payload->resize(hdr.payload_len);
    if (hdr.payload_len > 0 && !recv_all(fd, payload->data(), hdr.payload_len)) {
        return false;
    }

    *type = static_cast<MsgType>(hdr.type);
    *seq_no = hdr.seq_no;
    return true;
}

bool recv_msg_text(int fd, MsgType* type, std::string* payload, uint32_t* seq_no) {
    std::vector<uint8_t> raw;
    if (!recv_msg(fd, type, &raw, seq_no)) return false;
    payload->assign(raw.begin(), raw.end());
    return true;
}

std::vector<std::string> split_fields(const std::string& payload) {
    std::vector<std::string> fields;
    if (payload.empty()) return fields;

    std::stringstream ss(payload);
    std::string field;
    while (std::getline(ss, field, '|')) {
        fields.push_back(field);
    }
    // Handle a trailing '|' meaning an explicit empty final field.
    if (!payload.empty() && payload.back() == '|') {
        fields.push_back("");
    }
    return fields;
}

std::string join_fields(const std::vector<std::string>& fields) {
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out += '|';
        out += fields[i];
    }
    return out;
}
