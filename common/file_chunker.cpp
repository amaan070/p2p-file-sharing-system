#include "file_chunker.h"

#include <cstdio>
#include <fstream>

#include "protocol.h"
#include "sha1.h"

uint32_t compute_piece_count(uint64_t file_size) {
    if (file_size == 0) return 1; // an empty file is still "1 piece" (empty)
    return static_cast<uint32_t>((file_size + PIECE_SIZE - 1) / PIECE_SIZE);
}

bool compute_file_metadata(const std::string& path, FileMetadata* out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    uint64_t file_size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    out->file_size = file_size;
    out->piece_hashes.clear();

    SHA1 whole_file_hasher;
    std::vector<char> buffer(PIECE_SIZE);

    uint64_t remaining = file_size;
    // Even a zero-byte file produces exactly one (empty) piece hash, to
    // keep piece_hashes.size() consistent with compute_piece_count().
    do {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(PIECE_SIZE, remaining));
        if (to_read > 0) {
            file.read(buffer.data(), static_cast<std::streamsize>(to_read));
            std::streamsize got = file.gcount();
            if (static_cast<size_t>(got) != to_read) {
                return false; // file shrank/changed under us or read error
            }
        }

        SHA1 piece_hasher;
        if (to_read > 0) {
            piece_hasher.update(reinterpret_cast<const uint8_t*>(buffer.data()), to_read);
            whole_file_hasher.update(reinterpret_cast<const uint8_t*>(buffer.data()), to_read);
        }
        out->piece_hashes.push_back(piece_hasher.finalize_hex());

        remaining -= to_read;
    } while (remaining > 0);

    out->full_hash = whole_file_hasher.finalize_hex();
    return true;
}

bool read_piece(const std::string& path, uint32_t piece_index, uint64_t file_size,
                 std::vector<uint8_t>* out) {
    uint32_t piece_count = compute_piece_count(file_size);
    if (piece_index >= piece_count) {
        return false; // out of range
    }

    uint64_t offset = static_cast<uint64_t>(piece_index) * PIECE_SIZE;
    uint64_t remaining_in_file = file_size - offset;
    size_t expected_len = static_cast<size_t>(std::min<uint64_t>(PIECE_SIZE, remaining_in_file));

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file) {
        return false;
    }

    out->resize(expected_len);
    if (expected_len > 0) {
        file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(expected_len));
        if (static_cast<size_t>(file.gcount()) != expected_len) {
            return false; // truncated/corrupted local file
        }
    }
    return true;
}

bool get_file_size(const std::string& path, uint64_t* out_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    *out_size = static_cast<uint64_t>(file.tellg());
    return true;
}

bool preallocate_file(const std::string& path, uint64_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    if (size > 0) {
        file.seekp(static_cast<std::streamoff>(size - 1));
        char zero = 0;
        file.write(&zero, 1);
    }
    return static_cast<bool>(file);
}

bool write_piece(const std::string& path, uint32_t piece_index, const std::vector<uint8_t>& data) {
    // Open for read+write without truncating if the file already exists
    // (so previously-written pieces at other offsets are preserved); create
    // it first if it doesn't exist yet.
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        // File doesn't exist yet -- create it, then reopen for read+write.
        std::ofstream create(path, std::ios::binary);
        if (!create.is_open()) {
            return false;
        }
        create.close();
        file.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open()) {
            return false;
        }
    }

    uint64_t offset = static_cast<uint64_t>(piece_index) * PIECE_SIZE;
    file.seekp(static_cast<std::streamoff>(offset));
    if (!file) {
        return false;
    }

    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return file.good() || file.eof() == false; // write() failure sets failbit, not eof
}
