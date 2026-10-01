#ifndef CLIENT_FILE_REGISTRY_H
#define CLIENT_FILE_REGISTRY_H

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Thread-safe registry mapping (group_id, filename) -> local file path, for
// files this client is currently seeding. Populated when an upload
// succeeds and when a download completes. Read by the
// seeder's connection handler threads to locate the actual bytes to serve
// when a peer requests a piece.
class FileRegistry {
public:
    void register_file(const std::string& group_id, const std::string& filename,
                        const std::string& local_path);

    // Returns true and fills `out_path` if this client is sharing
    // (group_id, filename); false otherwise.
    bool lookup(const std::string& group_id, const std::string& filename,
                std::string* out_path) const;

    void unregister(const std::string& group_id, const std::string& filename);

    // Snapshot of everything currently shared, as (group_id, filename) pairs.
    std::vector<std::pair<std::string, std::string>> list() const;

private:
    static std::string make_key(const std::string& group_id, const std::string& filename);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> map_; // key -> local_path
};

#endif // CLIENT_FILE_REGISTRY_H
