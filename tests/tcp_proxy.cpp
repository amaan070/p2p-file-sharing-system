// tcp_proxy: a tiny TCP relay used ONLY by the failure-injection tests to
// simulate a network partition on one link (e.g. tracker <-> tracker) while
// everything else keeps working.
//
//   ./tcp_proxy <listen_port> <target_ip> <target_port>
//
// Normally it forwards every connection to the target, byte for byte.
//   SIGUSR1  -> PARTITION: closes every relayed connection immediately and
//               stops listening, so new connection attempts are refused.
//   SIGUSR2  -> HEAL: starts listening and forwarding again.
//
// Why a proxy: the two trackers dial each other using the addresses in their
// tracker_info file. Giving each tracker a private info file whose "peer" line
// points at a proxy lets the test cut exactly the tracker<->tracker link while
// both trackers stay reachable by clients (a genuine split-brain), which
// killing a process cannot do.
//
// Build: g++ -std=c++17 -Wall -Wextra -pthread tcp_proxy.cpp -o tcp_proxy

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static volatile sig_atomic_t g_want_partition = 0;
static volatile sig_atomic_t g_want_heal = 0;
static void on_usr1(int) { g_want_partition = 1; }
static void on_usr2(int) { g_want_heal = 1; }

static std::mutex g_mu;
static std::set<int> g_fds;          // every fd currently being relayed
static std::atomic<bool> g_partitioned{false};

static void track(int fd)   { std::lock_guard<std::mutex> l(g_mu); g_fds.insert(fd); }
static void untrack(int fd) { std::lock_guard<std::mutex> l(g_mu); g_fds.erase(fd); }

// Cuts every live connection. shutdown() (not close) so the relay threads that
// own the fds wake up, see EOF, and clean up themselves.
static void kill_all() {
    std::lock_guard<std::mutex> l(g_mu);
    for (int fd : g_fds) shutdown(fd, SHUT_RDWR);
}

static void relay(int a, int b) {
    char buf[65536];
    pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
    bool open = true;
    while (open && !g_partitioned.load()) {
        int r = poll(p, 2, 200);
        if (r < 0) break;
        for (int i = 0; i < 2 && open; ++i) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int from = p[i].fd, to = p[1 - i].fd;
            ssize_t n = recv(from, buf, sizeof(buf), 0);
            if (n <= 0) { open = false; break; }
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t w = send(to, buf + sent, static_cast<size_t>(n - sent), MSG_NOSIGNAL);
                if (w <= 0) { open = false; break; }
                sent += w;
            }
        }
    }
    untrack(a);
    untrack(b);
    close(a);
    close(b);
}

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || listen(fd, 32) < 0) {
        perror("tcp_proxy: bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_target(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <listen_port> <target_ip> <target_port>\n", argv[0]);
        return 1;
    }
    int listen_port = atoi(argv[1]);
    std::string target_ip = argv[2];
    int target_port = atoi(argv[3]);

    struct sigaction sa{};
    sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, nullptr);
    sa.sa_handler = on_usr2; sigaction(SIGUSR2, &sa, nullptr);

    int lfd = make_listener(listen_port);
    if (lfd < 0) return 1;
    fprintf(stderr, "tcp_proxy: :%d -> %s:%d (SIGUSR1 = partition, SIGUSR2 = heal)\n", listen_port, target_ip.c_str(), target_port);

    while (true) {
        if (g_want_partition) {
            g_want_partition = 0;
            g_partitioned = true;
            if (lfd >= 0) { close(lfd); lfd = -1; }   // refuse new connections
            kill_all();                               // cut existing ones
            fprintf(stderr, "tcp_proxy: PARTITIONED\n");
        }
        if (g_want_heal) {
            g_want_heal = 0;
            if (lfd < 0) lfd = make_listener(listen_port);
            g_partitioned = false;
            fprintf(stderr, "tcp_proxy: HEALED\n");
        }
        if (lfd < 0) { usleep(50000); continue; }

        pollfd p{lfd, POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r <= 0 || !(p.revents & POLLIN)) continue;

        int c = accept(lfd, nullptr, nullptr);
        if (c < 0) continue;
        int t = connect_target(target_ip, target_port);
        if (t < 0) { close(c); continue; }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(t, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        track(c);
        track(t);
        std::thread(relay, c, t).detach();
    }
}
