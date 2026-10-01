#ifndef COMMON_NETWORK_UTILS_H
#define COMMON_NETWORK_UTILS_H

#include <string>
#include <vector>
#include "protocol.h"

// ---------------------------------------------------------------------------
// Reliable, framed message I/O over TCP sockets.
//
// TCP is a byte stream -- a single send() on one side may arrive as several
// recv() calls on the other side, or several sends may be coalesced into one
// recv(). These helpers loop until the exact requested number of bytes has
// been transferred, so callers never have to think about partial reads.
// ---------------------------------------------------------------------------

// Disables Nagle's algorithm on a TCP socket (TCP_NODELAY). Our protocol is
// request/response with whole messages already assembled in one write, so
// there is nothing to gain from Nagle's batching -- it can only add latency
// (it may hold back the last partial segment of a message waiting for an ACK).
// Best-effort: failure is harmless and ignored.
void set_tcp_nodelay(int fd);

// Sends exactly `len` bytes from `buf` on `fd`. Loops internally to handle
// partial writes and EINTR. Returns true on success, false on error/closed
// connection.
bool send_all(int fd, const void* buf, size_t len);

// Receives exactly `len` bytes into `buf` from `fd`. Loops internally to
// handle partial reads and EINTR. Returns true on success, false if the
// connection closed before all bytes arrived or on error.
bool recv_all(int fd, void* buf, size_t len);

// Sends a full framed message: header (magic/type/payload_len/seq_no) +
// payload bytes. Returns true on success.
bool send_msg(int fd, MsgType type, const std::string& payload, uint32_t seq_no = 0);

// Sends a full framed message with a raw binary payload (used for piece
// data). Returns true on success.
bool send_msg_bytes(int fd, MsgType type, const std::vector<uint8_t>& payload, uint32_t seq_no = 0);

// Receives one full framed message. Reads the header first, validates the
// magic number and payload_len bound, then reads exactly that many payload
// bytes. Returns true on success; false on malformed header, oversized
// payload, or connection closed/error. On success, `type`, `payload`, and
// `seq_no` are populated. `payload` may contain binary data (piece bytes),
// so it is returned as a vector<uint8_t> rather than a string.
bool recv_msg(int fd, MsgType* type, std::vector<uint8_t>* payload, uint32_t* seq_no);

// Convenience wrapper around recv_msg for text (delimited) payloads.
bool recv_msg_text(int fd, MsgType* type, std::string* payload, uint32_t* seq_no);

// Splits a '|'-delimited payload string into fields. Empty input yields an
// empty vector, not a vector with one empty string.
std::vector<std::string> split_fields(const std::string& payload);

// Joins fields with '|' into a single payload string.
std::string join_fields(const std::vector<std::string>& fields);

#endif // COMMON_NETWORK_UTILS_H
