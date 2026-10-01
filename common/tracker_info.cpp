#include "tracker_info.h"

#include <fstream>
#include <sstream>

bool parse_ip_port(const std::string& s, TrackerAddr* out) {
    size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon == s.size() - 1) {
        return false;
    }
    std::string ip = s.substr(0, colon);
    std::string port_str = s.substr(colon + 1);

    try {
        size_t consumed = 0;
        int port = std::stoi(port_str, &consumed);
        if (consumed != port_str.size() || port <= 0 || port > 65535) {
            return false;
        }
        out->ip = ip;
        out->port = port;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_tracker_info(const std::string& path, std::vector<TrackerAddr>* out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    out->clear();
    std::string line;
    while (std::getline(file, line)) {
        // Trim trailing carriage return (Windows line endings) and whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue; // blank line
        line = line.substr(start);

        if (line.empty() || line[0] == '#') continue; // comment/blank

        TrackerAddr addr;
        if (!parse_ip_port(line, &addr)) {
            return false; // malformed line -> whole file considered invalid
        }
        out->push_back(addr);
    }

    return !out->empty();
}
