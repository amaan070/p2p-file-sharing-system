#include "download_manager.h"

#include <iostream>
#include <sstream>

#include "tracker_client.h"

DownloadManager::DownloadManager(FileRegistry* registry, TrackerClient* tracker)
    : registry_(registry), tracker_(tracker) {}

DownloadManager::~DownloadManager() {
    // A std::thread that is destroyed while still joinable calls
    // std::terminate(), so we must never leave a running download behind.
    wait_all();
}

bool DownloadManager::conflict_locked(const std::string& group_id, const std::string& filename,
                                      const std::string& dest_path, std::string* reason) const {
    size_t active = 0;
    for (const auto& e : entries_) {
        if (e->state.load() != DOWNLOADING) continue;
        ++active;
        if (e->group_id == group_id && e->filename == filename) {
            *reason = "'" + filename + "' from group '" + group_id + "' is already being downloaded.";
            return true;
        }
        // Two downloads writing the same destination file would corrupt each
        // other (both pre-allocate / truncate it and write pieces into it).
        if (e->dest_path == dest_path) {
            *reason = "destination '" + dest_path + "' is already in use by a running download.";
            return true;
        }
    }
    if (active >= kMaxActiveDownloads) {
        *reason = "too many downloads in progress (limit " + std::to_string(kMaxActiveDownloads) +
                  "); wait for one to finish.";
        return true;
    }
    return false;
}

bool DownloadManager::can_start(const std::string& group_id, const std::string& filename,
                                const std::string& dest_path, std::string* reason) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !conflict_locked(group_id, filename, dest_path, reason);
}

bool DownloadManager::start(const std::string& group_id, const std::string& filename,
                            const std::string& dest_path, const DownloadInfo& info,
                            std::string* reason) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Re-check under the lock: can_start() may have been answered a moment ago.
    if (conflict_locked(group_id, filename, dest_path, reason)) return false;

    // If this same file was downloaded (or failed) before, drop the old row so
    // `show downloads` reflects the latest attempt instead of piling up
    // history. Its thread has already finished its work (state is the last
    // thing it sets), so join() returns almost instantly.
    for (auto it = entries_.begin(); it != entries_.end();) {
        Entry* old = it->get();
        if (old->state.load() != DOWNLOADING && old->group_id == group_id && old->filename == filename) {
            if (old->thread.joinable()) old->thread.join();
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    auto entry = std::make_unique<Entry>();
    entry->group_id = group_id;
    entry->filename = filename;
    entry->dest_path = dest_path;
    entry->info = info;
    entry->progress.total_pieces.store(static_cast<uint32_t>(info.piece_hashes.size()));

    Entry* raw = entry.get(); // stable address: the Entry lives on the heap
    entries_.push_back(std::move(entry));

    try {
        raw->thread = std::thread(&DownloadManager::run, this, raw);
    } catch (const std::exception& ex) {
        // Thread creation can fail when the system is out of resources.
        entries_.pop_back();
        *reason = std::string("could not start download thread: ") + ex.what();
        return false;
    }
    return true;
}

void DownloadManager::run(Entry* e) {
    // Nothing may escape a thread body: an uncaught exception there calls
    // std::terminate() and kills the whole client.
    bool ok = false;
    try {
        ok = download_file_multi_peer(e->group_id, e->filename, e->info, e->dest_path,
                                      /*max_workers=*/4, &e->progress);
    } catch (...) {
        ok = false;
    }

    if (ok && registry_) {
        try {
            // Order matters: make the file servable locally FIRST, then tell
            // the tracker. Otherwise another client could be sent to us before
            // our seeder can find the file and would get a NOT_FOUND.
            registry_->register_file(e->group_id, e->filename, e->dest_path);
            if (tracker_ && !tracker_->announce_seeder(e->group_id, e->filename)) {
                std::cout << "Warning: download succeeded but the tracker was not told "
                             "this client is now a seeder for " << e->filename << ".\n" << std::flush;
            }
        } catch (...) {
            // The file itself downloaded and verified fine; only the
            // announcement failed, so the download still counts as complete.
        }
    }

    // Last thing we do. `show downloads` only reports [C] once the file is
    // both verified on disk and registered for seeding.
    e->state.store(ok ? COMPLETED : FAILED);
}

void DownloadManager::print_downloads() const {
    // Build the whole report under the lock, print after releasing it, so a
    // slow terminal never holds up start()/wait_all().
    std::ostringstream out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.empty()) {
            out << "No downloads.\n";
        }
        for (const auto& e : entries_) {
            int st = e->state.load();
            if (st == COMPLETED) {
                out << "[C] [" << e->group_id << "] " << e->filename << "\n";
            } else if (st == FAILED) {
                out << "[F] [" << e->group_id << "] " << e->filename << " (failed)\n";
            } else {
                uint32_t done = e->progress.completed_pieces.load();
                uint32_t total = e->progress.total_pieces.load();
                out << "[D] [" << e->group_id << "] " << e->filename << " (" << done << "/" << total
                    << " pieces";
                if (total > 0) out << ", " << (static_cast<uint64_t>(done) * 100 / total) << "%";
                out << ")\n";
            }
        }
    }
    std::cout << out.str() << std::flush;
}

size_t DownloadManager::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& e : entries_) {
        if (e->state.load() == DOWNLOADING) ++n;
    }
    return n;
}

void DownloadManager::wait_all() {
    // Take the thread handles out while holding the lock, but join them after
    // releasing it. Joining can take a long time (a big download) and we do
    // not want to hold the table lock for that long.
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_) {
            if (e->thread.joinable()) to_join.push_back(std::move(e->thread));
        }
    }
    for (auto& t : to_join) t.join();
}
