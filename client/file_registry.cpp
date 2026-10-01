#include "file_registry.h"

// Unit separator (0x1F) as the key delimiter -- practically never appears
// in a group_id or filename, unlike '|' or '/' which could plausibly show
// up in a filename.
std::string FileRegistry::make_key(const std::string& group_id, const std::string& filename) {
    return group_id + '\x1f' + filename;
}

void FileRegistry::register_file(const std::string& group_id, const std::string& filename,
                                  const std::string& local_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[make_key(group_id, filename)] = local_path;
}

bool FileRegistry::lookup(const std::string& group_id, const std::string& filename,
                           std::string* out_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(make_key(group_id, filename));
    if (it == map_.end()) return false;
    *out_path = it->second;
    return true;
}

std::vector<std::pair<std::string, std::string>> FileRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(map_.size());
    for (const auto& kv : map_) {
        size_t sep = kv.first.find('\x1f');
        if (sep == std::string::npos) continue;
        out.emplace_back(kv.first.substr(0, sep), kv.first.substr(sep + 1));
    }
    return out;
}

void FileRegistry::unregister(const std::string& group_id, const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.erase(make_key(group_id, filename));
}
