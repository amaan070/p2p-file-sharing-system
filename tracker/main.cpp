// ./tracker tracker_info.txt tracker_no
//
// tracker_no is 1 or 2, selecting which line of tracker_info.txt this
// instance binds to.

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../common/network_utils.h"
#include "../common/tracker_info.h"
#include "connection_handler.h"
#include "replicator.h"
#include "tracker_state.h"

// Global flag used to signal shutdown from the console-reader thread to the
// main accept loop. atomic<bool> is safe to read/write across threads
// without an explicit mutex for this simple on/off flag.
std::atomic<bool> g_shutdown_requested{false};
// Set by console_loop when it returns (after "quit" or end of stdin), so main
// knows whether joining that thread would block.
std::atomic<bool> g_console_done{false};

// SIGINT / SIGTERM request the same graceful shutdown as typing "quit". The
// handler only stores to a lock-free atomic, which is async-signal-safe.
static void on_terminate_signal(int) { g_shutdown_requested = true; }

static int create_listener_socket(const std::string& ip, int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    // Allow immediate re-bind after restart (avoids "address already in use"
    // during development/testing when the previous instance just exited).
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (ip == "0.0.0.0" || ip.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid IP address in tracker_info.txt: " << ip << "\n";
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 32) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// Reads commands from stdin (currently just "quit") and sets the shutdown
// flag. Runs on its own thread so it doesn't block the accept() loop.
static void console_loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") {
            std::cout << "Shutting down tracker...\n";
            g_shutdown_requested = true;
            break;
        } else if (!line.empty()) {
            std::cout << "Unknown console command. Type 'quit' to shut down.\n";
        }
    }
    g_console_done = true;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <tracker_info.txt> <tracker_no>\n";
        return 1;
    }

    std::string info_path = argv[1];
    int tracker_no = std::atoi(argv[2]);
    if (tracker_no != 1 && tracker_no != 2) {
        std::cerr << "tracker_no must be 1 or 2\n";
        return 1;
    }

    std::vector<TrackerAddr> trackers;
    if (!parse_tracker_info(info_path, &trackers)) {
        std::cerr << "Failed to parse tracker info file: " << info_path << "\n";
        return 1;
    }
    if (static_cast<int>(trackers.size()) < 2) {
        std::cerr << "tracker_info.txt must list exactly 2 trackers (found "
                   << trackers.size() << ")\n";
        return 1;
    }

    TrackerAddr self = trackers[tracker_no - 1];
    TrackerAddr peer = trackers[tracker_no == 1 ? 1 : 0];

    std::cout << "Starting tracker " << tracker_no << " on " << self.ip << ":" << self.port
              << " (peer tracker at " << peer.ip << ":" << peer.port << ")\n";

    int listen_fd = create_listener_socket(self.ip, self.port);
    if (listen_fd < 0) {
        return 1;
    }

    // A dying peer connection must not kill the tracker with SIGPIPE, and a
    // signal should shut us down cleanly rather than abruptly.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_terminate_signal);
    std::signal(SIGTERM, on_terminate_signal);

    TrackerState state;
    ConnectionSet conns;   // declared after `state`, so it is destroyed first
    state.set_tracker_no(tracker_no);

    // Sync with the peer tracker: live ops are streamed as they happen (the
    // sink runs under the state lock, see tracker_state.h); on every
    // (re)connect the full state is pushed; when the link drops, sessions
    // hosted by the peer are marked offline.
    Replicator replicator(
        peer,
        [&state]() { return state.snapshot_ops(); },
        [&state]() { state.mark_remote_users_offline(); });
    state.set_replication_sink([&replicator](const std::vector<std::string>& op) {
        replicator.enqueue(op);
    });
    replicator.start();

    std::thread console_thread(console_loop);

    std::cout << "Tracker ready. Type 'quit' to shut down.\n";

    // Accept loop with a short timeout so we can periodically check
    // g_shutdown_requested without blocking forever in accept().
    while (!g_shutdown_requested) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);

        timeval tv{};
        tv.tv_sec = 1; // check shutdown flag once per second
        tv.tv_usec = 0;

        int ready = select(listen_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ready == 0) continue; // timeout, loop back to check shutdown flag

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        set_tcp_nodelay(client_fd);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cout << "Accepted connection from " << ip_str << ":" << ntohs(client_addr.sin_port) << "\n";

        if (!conns.add(client_fd)) {   // shutting down
            close(client_fd);
            break;
        }
        try {
            std::thread(handle_client_connection, client_fd, &state, &conns).detach();
        } catch (const std::exception& ex) {
            // Out of threads: drop just this connection and keep serving the rest.
            std::cerr << "could not start handler thread: " << ex.what() << "\n";
            conns.done(client_fd);
        }
    }

    close(listen_fd);

    // Disconnect every client and wait for their handler threads to finish.
    // Until this returns, those threads still use `state` and (through its
    // replication sink) the replicator, so neither may be torn down yet.
    conns.shutdown_and_wait();

    // Detach the sink FIRST (set_replication_sink takes the state lock, so
    // once it returns no in-flight mutation can still call into the
    // replicator), then stop the sender thread.
    state.set_replication_sink(nullptr);
    replicator.stop();

    // The console thread ends after a "quit" line or EOF on stdin. If we are
    // here because of a signal it may still be blocked reading stdin: joining
    // would hang, so let it go (the process is exiting anyway).
    if (console_thread.joinable()) {
        if (g_console_done) console_thread.join();
        else console_thread.detach();
    }

    std::cout << "Tracker stopped.\n";
    return 0;
}
