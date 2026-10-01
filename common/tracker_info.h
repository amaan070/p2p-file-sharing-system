#ifndef COMMON_TRACKER_INFO_H
#define COMMON_TRACKER_INFO_H

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// tracker_info.txt format (our own design, since the assignment leaves this
// unspecified):
//
//   <ip>:<port>
//   <ip>:<port>
//
// Exactly one line per tracker, in order (line 1 = tracker_no 1, line 2 =
// tracker_no 2). Blank lines and lines starting with '#' are ignored, so the
// file can carry comments, e.g.:
//
//   # Tracker configuration for the P2P file sharing system
//   127.0.0.1:9001
//   127.0.0.1:9002
// ---------------------------------------------------------------------------

struct TrackerAddr {
    std::string ip;
    int port;
};

// Parses a tracker_info.txt file. Returns true on success and fills `out`
// with one entry per tracker line, in file order. Returns false if the file
// can't be opened or a line is malformed.
bool parse_tracker_info(const std::string& path, std::vector<TrackerAddr>* out);

// Parses a single "ip:port" string (used for the client's own listen
// address argument). Returns true on success.
bool parse_ip_port(const std::string& s, TrackerAddr* out);

#endif // COMMON_TRACKER_INFO_H
