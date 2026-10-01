#ifndef COMMON_FILE_CHUNKER_H
#define COMMON_FILE_CHUNKER_H

#include <cstdint>
#include <string>
#include <vector>

// Utilities for splitting files into fixed-size pieces (PIECE_SIZE, see
// protocol.h), computing per-piece and whole-file SHA1 hashes, and reading/
// writing individual pieces by index. Designed to handle files up to 1GB
// without loading more than one piece into memory at a time.

struct FileMetadata {
    uint64_t file_size = 0;
    std::string full_hash;                 // SHA1 of the entire file
    std::vector<std::string> piece_hashes; // SHA1 of each PIECE_SIZE chunk (last may be smaller)
};

// Computes the number of pieces a file of `file_size` bytes splits into.
// A zero-byte file still counts as 1 piece (an empty one) for simplicity.
uint32_t compute_piece_count(uint64_t file_size);

// Reads the file at `path` once, computing both the whole-file SHA1 and the
// SHA1 of each individual PIECE_SIZE-aligned chunk, without ever holding
// more than one piece's worth of data in memory. Returns false if the file
// cannot be opened.
bool compute_file_metadata(const std::string& path, FileMetadata* out);

// Reads exactly piece `piece_index` (0-based) from the file at `path` into
// `out`. `out` is resized to the actual number of bytes read (PIECE_SIZE
// for all but possibly the last piece). Returns false if the file can't be
// opened, the piece index is out of range, or fewer bytes than expected
// were available (indicating a truncated/corrupted local file).
bool read_piece(const std::string& path, uint32_t piece_index, uint64_t file_size,
                 std::vector<uint8_t>* out);
                 
// Returns the size in bytes of the file at `path`. Returns false if the
// file cannot be opened.
bool get_file_size(const std::string& path, uint64_t* out_size);

// Creates (or truncates) the file at `path` and extends it to exactly
// `size` bytes, without writing any actual piece content. Used before
// starting concurrent multi-peer piece writes, so multiple threads can
// safely open independent file handles and seek/write to disjoint,
// already-allocated byte ranges of the same file without racing on file
// creation or on-demand extension. Returns false on I/O failure.
bool preallocate_file(const std::string& path, uint64_t size);

// Writes `data` as piece `piece_index` into the file at `path`, seeking to
// the correct offset. Creates the file if it doesn't exist and extends it
// as needed (later pieces may be written before earlier ones arrive, since
// pieces can be downloaded out of order from different peers). Returns
// false on any I/O failure.
bool write_piece(const std::string& path, uint32_t piece_index, const std::vector<uint8_t>& data);

#endif // COMMON_FILE_CHUNKER_H
