#include "sha1.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
inline uint32_t rol(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}
}

SHA1::SHA1() : total_len_(0), buffer_len_(0) {
    h_[0] = 0x67452301;
    h_[1] = 0xEFCDAB89;
    h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476;
    h_[4] = 0xC3D2E1F0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void SHA1::process_block(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void SHA1::update(const uint8_t* data, size_t len) {
    total_len_ += len;

    size_t offset = 0;
    // Fill any partial buffer left over from a previous update() first.
    if (buffer_len_ > 0) {
        size_t needed = 64 - buffer_len_;
        size_t take = std::min(needed, len);
        std::memcpy(buffer_ + buffer_len_, data, take);
        buffer_len_ += take;
        offset += take;
        if (buffer_len_ == 64) {
            process_block(buffer_);
            buffer_len_ = 0;
        }
    }

    // Process full 64-byte blocks directly from the input.
    while (offset + 64 <= len) {
        process_block(data + offset);
        offset += 64;
    }

    // Stash any remainder for the next update() or finalize().
    size_t remaining = len - offset;
    if (remaining > 0) {
        std::memcpy(buffer_ + buffer_len_, data + offset, remaining);
        buffer_len_ += remaining;
    }
}

void SHA1::update(const std::string& data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::array<uint8_t, 20> SHA1::finalize() {
    uint64_t bit_len = total_len_ * 8;

    // Padding: a single 0x80 byte, then zeros, then the 8-byte bit length,
    // such that the total padded length is a multiple of 64 bytes.
    uint8_t pad = 0x80;
    update(&pad, 1);

    uint8_t zero = 0x00;
    while (buffer_len_ != 56) {
        update(&zero, 1);
    }

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    }
    // Bypass update()'s length tracking for the length field itself by
    // writing directly into the buffer and processing the final block.
    std::memcpy(buffer_ + 56, len_bytes, 8);
    process_block(buffer_);
    buffer_len_ = 0;

    std::array<uint8_t, 20> digest;
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
    return digest;
}

std::string SHA1::finalize_hex() {
    auto digest = finalize();
    char hex[41];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 40);
}

std::string SHA1::hash_hex(const uint8_t* data, size_t len) {
    SHA1 sha;
    sha.update(data, len);
    return sha.finalize_hex();
}

std::string SHA1::hash_hex(const std::string& data) {
    return hash_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool SHA1::hash_file_hex(const std::string& path, std::string* out_hex, size_t chunk_size) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    SHA1 sha;
    std::vector<char> buffer(chunk_size);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = file.gcount();
        if (got > 0) {
            sha.update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(got));
        }
    }

    *out_hex = sha.finalize_hex();
    return true;
}
