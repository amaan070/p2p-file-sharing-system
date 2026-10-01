#ifndef CLIENT_DOWNLOAD_MANAGER_H
#define CLIENT_DOWNLOAD_MANAGER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "downloader.h"
#include "file_registry.h"

class TrackerClient; // only needed to announce ourselves as a seeder afterwards

// Runs several downloads at the same time and remembers each one so that
// `show downloads` can report on it.
//
// Analogy: a restaurant floor manager. The main thread (the waiter at the
// prompt) hands each new order to the manager, who gives it to its own cook
// (a thread) and writes it on a clipboard (the table below). The waiter is
// immediately free to take the next command, and `show downloads` is just a
// glance at the clipboard.
//
// Threading rules (these are what make it safe -- worth knowing for the viva):
//   * mutex_ guards the *table itself* (adding/removing rows, iterating).
//   * A row's progress counters and state are std::atomic, so a download
//     thread updates them WITHOUT taking mutex_. Consequently a download
//     thread can never block the prompt, and the prompt can never block a
//     download thread, and there is no lock-ordering problem between them.
//   * A row is only destroyed after its thread has been join()ed, so a
//     worker never touches freed memory.
class DownloadManager {
public:
    // `registry` and `tracker` must outlive this object. Either may be null
    // (used by tests that only exercise the table).
    DownloadManager(FileRegistry* registry, TrackerClient* tracker);
    ~DownloadManager(); // waits for every running download (calls wait_all)
    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    // Upper bound on simultaneously running downloads. Each one uses up to
    // four worker threads plus sockets, so an unbounded number could
    // exhaust threads / file descriptors.
    static constexpr size_t kMaxActiveDownloads = 16;

    // Would a new download of (group_id, filename) -> dest_path be refused
    // right now? If so returns false and fills *reason. Lets the caller fail
    // fast *before* bothering the tracker. start() re-checks atomically.
    bool can_start(const std::string& group_id, const std::string& filename,
                   const std::string& dest_path, std::string* reason) const;

    // Registers the download in the table and starts its thread. Returns
    // immediately. Returns false (with *reason) if it is refused: same file
    // already downloading, destination already in use by a running download,
    // too many running downloads, or the thread could not be created.
    bool start(const std::string& group_id, const std::string& filename,
               const std::string& dest_path, const DownloadInfo& info, std::string* reason);

    // `show downloads`: prints one line per download.
    //   [C] [group_id] filename            completed (and now seeding)
    //   [D] [group_id] filename (x/y pieces, p%)   in progress
    //   [F] [group_id] filename (failed)   gave up
    void print_downloads() const;

    // Number of downloads still running.
    size_t active_count() const;

    // Blocks until every running download has finished (either way).
    void wait_all();

private:
    enum State { DOWNLOADING = 0, COMPLETED = 1, FAILED = 2 };

    struct Entry {
        std::string group_id, filename, dest_path;
        DownloadInfo info;              // immutable once the thread starts
        DownloadProgress progress;      // atomics, written by the download thread
        std::atomic<int> state{DOWNLOADING};
        std::thread thread;
    };

    // Thread body for one download.
    void run(Entry* e);

    // Requires mutex_ held.
    bool conflict_locked(const std::string& group_id, const std::string& filename,
                         const std::string& dest_path, std::string* reason) const;

    FileRegistry* registry_;
    TrackerClient* tracker_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_; // guarded by mutex_
};

#endif // CLIENT_DOWNLOAD_MANAGER_H
