// ./client <IP>:<PORT> tracker_info.txt
//
// <IP>:<PORT> is the address THIS client listens on for incoming peer
// connections (i.e. its seeder socket), used when other clients download
// pieces from it. tracker_info.txt gives the addresses of both trackers so
// the client can connect (and fail over between them).

#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "../common/tracker_info.h"
#include "tracker_client.h"
#include "seeder.h"
#include "file_registry.h"
#include "downloader.h"
#include "download_manager.h"

struct CommandLine {
    std::string command;
    std::vector<std::string> args;
};

static CommandLine parse_command_line(const std::string& line) {
    CommandLine cl;
    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> tokens;
    while (iss >> token) {
        tokens.push_back(token);
    }
    if (tokens.empty()) return cl;

    static const std::vector<std::string> two_word_commands = {
        "create user", "create group", "join group", "leave group",
        "list groups", "list requests", "list files", "accept request",
        "upload file", "download file", "show downloads", "stop share",
    };

    if (tokens.size() >= 2) {
        std::string first_two = tokens[0] + " " + tokens[1];
        for (const auto& cmd : two_word_commands) {
            if (first_two == cmd) {
                cl.command = cmd;
                cl.args.assign(tokens.begin() + 2, tokens.end());
                return cl;
            }
        }
    }

    cl.command = tokens[0];
    cl.args.assign(tokens.begin() + 1, tokens.end());
    return cl;
}

// `download file <group_id> <file_name> <destination_path>`
//
// Asking the tracker "who has this file, and what are its hashes?" is a single
// quick request, so we do it right here on the prompt thread. That way
// mistakes (not a member, no such file, no tracker) are reported immediately.
// Only the slow part -- actually moving the bytes -- is handed to the
// DownloadManager, which runs it on its own thread and returns at once, so the
// prompt is free for the next command.
static void handle_download(TrackerClient& tracker_client, DownloadManager& downloads,
                            const std::vector<std::string>& args) {
    if (args.size() != 3) {
        std::cout << "Usage: download file <group_id> <file_name> <destination_path>\n";
        return;
    }
    const std::string& group_id = args[0];
    const std::string& filename = args[1];

    // <destination path> may name the file to create, or an existing directory,
    // in which case the file is saved inside it under its own name. (The spec
    // does not say which is meant, so both are accepted.)
    std::string dest_path = args[2];
    struct stat st;
    if (stat(dest_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        // The name comes from the tracker, i.e. from another user: never let it
        // climb out of the directory the user chose.
        if (filename.find('/') != std::string::npos || filename == "." || filename == "..") {
            std::cout << "ERROR: refusing unsafe file name '" << filename << "'.\n";
            return;
        }
        if (dest_path.back() != '/') dest_path += '/';
        dest_path += filename;
    } else if (dest_path.back() == '/') {
        std::cout << "ERROR: directory '" << dest_path << "' does not exist.\n";
        return;
    }

    std::string reason;
    if (!downloads.can_start(group_id, filename, dest_path, &reason)) {
        std::cout << "ERROR: " << reason << "\n";  // fail fast, don't bother the tracker
        return;
    }

    DownloadInfo info;
    if (!tracker_client.fetch_download_info(group_id, filename, &info)) return;

    if (!downloads.start(group_id, filename, dest_path, info, &reason)) {
        std::cout << "ERROR: " << reason << "\n";
        return;
    }
    std::cout << "Download started: [" << group_id << "] " << filename
              << " -- use 'show downloads' to see progress.\n";
}

// Arguments travel inside '|'-separated protocol messages, so a '|' inside one
// would be misread as a field boundary. Reject it (and absurd lengths) up front
// with a clear message instead of sending something the tracker would misparse.
static bool arguments_are_valid(const std::vector<std::string>& args) {
    const size_t kMaxArgLen = 1024;
    for (const auto& a : args) {
        if (a.find('|') != std::string::npos) {
            std::cout << "Error: arguments may not contain '|' (it is the protocol's field separator).\n";
            return false;
        }
        if (a.size() > kMaxArgLen) {
            std::cout << "Error: argument too long (limit " << kMaxArgLen << " characters).\n";
            return false;
        }
    }
    return true;
}

// Blocks until all running downloads are done, telling the user if it has to wait.
static void wait_for_downloads(DownloadManager& downloads) {
    size_t running = downloads.active_count();
    if (running > 0) {
        std::cout << "Waiting for " << running << " active download"
                  << (running == 1 ? "" : "s") << " to finish...\n" << std::flush;
    }
    downloads.wait_all();
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <IP>:<PORT> <tracker_info.txt>\n";
        return 1;
    }

    TrackerAddr self_addr;
    if (!parse_ip_port(argv[1], &self_addr)) {
        std::cerr << "Invalid <IP>:<PORT>: " << argv[1] << "\n";
        return 1;
    }

    std::vector<TrackerAddr> trackers;
    if (!parse_tracker_info(argv[2], &trackers)) {
        std::cerr << "Failed to parse tracker info file: " << argv[2] << "\n";
        return 1;
    }
    if (static_cast<int>(trackers.size()) < 2) {
        std::cerr << "tracker_info.txt must list exactly 2 trackers (found "
                   << trackers.size() << ")\n";
        return 1;
    }

    std::cout << "Client listening address: " << self_addr.ip << ":" << self_addr.port << "\n";
    std::cout << "Known trackers:\n";
    for (size_t i = 0; i < trackers.size(); ++i) {
        std::cout << "  tracker " << (i + 1) << ": " << trackers[i].ip << ":" << trackers[i].port << "\n";
    }

    TrackerClient tracker_client;
    if (!tracker_client.connect_to_trackers(trackers)) {
        return 1;
    }

    tracker_client.start_monitor(); // background failover + automatic re-login

    FileRegistry file_registry;
    tracker_client.set_file_registry(&file_registry);
    if (!start_seeder(self_addr.ip, self_addr.port, &file_registry)) {
        std::cerr << "Warning: failed to start seeder listener -- this client cannot serve files to peers.\n";
    }

    // Declared AFTER tracker_client and file_registry on purpose: locals are
    // destroyed in reverse order, so the manager (whose download threads use
    // both) is torn down -- and its threads joined -- before either of them.
    DownloadManager downloads(&file_registry, &tracker_client);

    std::cout << "\nReady. Type a command (e.g. 'create user alice pw123'):\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        CommandLine cl = parse_command_line(line);
        if (cl.command.empty()) continue;

        if (cl.command == "logout") {
            // End the session and stop sharing: running downloads finish first
            // (they still need the session to announce themselves as seeders),
            // then every shared file is withdrawn from the tracker, then the
            // tracker session is closed. The client keeps running, so the user
            // can log in again (or someone else can).
            wait_for_downloads(downloads);
            tracker_client.unshare_all();
            tracker_client.logout();
            continue;
        }
        if (cl.command == "quit") {
            wait_for_downloads(downloads);
            if (tracker_client.logged_in()) {
                tracker_client.unshare_all();
                tracker_client.logout();
            }
            std::cout << "Exiting client.\n";
            break;
        }

        if (!arguments_are_valid(cl.args)) continue;

        if (cl.command == "create user") tracker_client.create_user(cl.args);
        else if (cl.command == "login") tracker_client.login(cl.args, self_addr.ip, self_addr.port);
        else if (cl.command == "create group") tracker_client.create_group(cl.args);
        else if (cl.command == "join group") tracker_client.join_group(cl.args);
        else if (cl.command == "leave group") tracker_client.leave_group(cl.args);
        else if (cl.command == "list groups") tracker_client.list_groups();
        else if (cl.command == "list requests") tracker_client.list_requests(cl.args);
        else if (cl.command == "accept request") tracker_client.accept_request(cl.args);
        else if (cl.command == "upload file") tracker_client.upload_file(cl.args);
        else if (cl.command == "list files") tracker_client.list_files(cl.args);
        else if (cl.command == "download file") handle_download(tracker_client, downloads, cl.args);
        else if (cl.command == "show downloads") downloads.print_downloads();
        else if (cl.command == "stop share") tracker_client.stop_share(cl.args);
        else std::cout << "Unknown command: " << cl.command << "\n";
    }

    // Reached on `quit` (already drained above) and also when stdin
    // ends (Ctrl-D, or a piped script running out of commands). In the
    // latter case downloads may still be running; finish them rather than
    // abandoning half-written files.
    downloads.wait_all();
    if (tracker_client.logged_in()) {   // input ended without an explicit logout/quit
        tracker_client.unshare_all();
        tracker_client.logout();
    }

    // Close the seeder: stop accepting peers, disconnect the ones being served
    // and wait for their threads to leave. Until this returns, those threads
    // still hold a pointer to file_registry, which is destroyed as soon as
    // main() returns -- so this must come first.
    stop_all_seeders();
    return 0;
}