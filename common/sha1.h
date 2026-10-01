#ifndef COMMON_SHA1_H
#define COMMON_SHA1_H

#include <array>
#include <cstdint>
#include <string>

// Self-contained SHA1 implementation (no external library dependency, per
// the assignment's "standard libraries only" constraint). Produces a
// 20-byte digest, exposed both as raw bytes and as a 40-character lowercase
// hex string.

class SHA1 {
public:
    SHA1();

    // Feeds more data into the running hash. Can be called multiple times
    // (e.g. once per file chunk) before calling finalize().
    void update(const uint8_t* data, size_t len);
    void update(const std::string& data);

    // Finalizes the hash and returns the 20-byte digest. After calling
    // this, the object should not be reused (create a new SHA1 instance
    // for a new hash).
    std::array<uint8_t, 20> finalize();

    // Convenience: finalize() and format as a 40-char lowercase hex string.
    std::string finalize_hex();

    // One-shot convenience for hashing an in-memory buffer.
    static std::string hash_hex(const uint8_t* data, size_t len);
    static std::string hash_hex(const std::string& data);

    // One-shot convenience for hashing a file on disk, reading it in
    // chunks (default 512KB, matching the piece size) rather than loading
    // the whole file into memory at once. Returns false if the file
    // cannot be opened.
    static bool hash_file_hex(const std::string& path, std::string* out_hex,
                               size_t chunk_size = 512 * 1024);

private:
    void process_block(const uint8_t* block);

    uint32_t h_[5];
    uint64_t total_len_;
    uint8_t buffer_[64];
    size_t buffer_len_;
};

#endif // COMMON_SHA1_H
