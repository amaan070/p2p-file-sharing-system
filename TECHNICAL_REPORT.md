# Technical Report: Peer-to-Peer Distributed File Sharing System


## 1. Overview and approach

The system is split by responsibility. **Trackers** hold metadata only (accounts, groups, file hashes,
who is seeding what) and never see file bytes. **Clients** hold the files and move them directly to each
other. This keeps the trackers small and cheap, and makes file transfer scale with the number of peers.

The design followed four principles that recur throughout:

1. **Bounded memory.** Nothing larger than one 512 KB piece is ever in memory, so a 1 GB file costs the
   same as a small one.
2. **Verify everything received.** Every piece is checked against its SHA-1 before it is written; the
   whole file is checked at the end. The system never trusts a peer.
3. **Make repeated operations harmless (idempotence).** Replicated operations can be replayed, and
   snapshots can be pushed at any time, without changing the result. This one property makes recovery,
   partitions and retries simple.
4. **Every thread that uses shared state must be joined before that state is destroyed.**

The code is about 4,300 lines (client 1,900, tracker 1,600, shared library 800) plus about 3,200 lines
of tests. Only POSIX sockets, `std::thread`, the standard library and the `stat()` system call are used;
SHA-1 is implemented from scratch.

## 2. Synchronisation algorithm

### 2.1 Requirements taken from the spec

Two trackers; a client must get accurate information from either; the system must keep working with one
tracker down; a tracker that returns must recover; temporary disconnection must be handled.

### 2.2 Design: operation-based replication, snapshot on reconnect

Each tracker has a **replicator** thread with one persistent connection to the other tracker.

* **Live path.** Each successful local mutation is handed to the replicator while the state lock is
  still held, so operations are queued in exactly the order they were applied. The replicator sends them
  one at a time and waits for an acknowledgement carrying the same sequence number. An idle link carries
  a heartbeat every second, so a silent peer is detected in a few seconds.
* **Apply path.** The receiving tracker applies the operation *without re-checking business rules*. The
  origin already validated it; re-validating on the other side would reject operations that were valid
  in the origin's ordering (for example an `accept_request` arriving before the tracker has seen the
  matching `request_join`). Apply only checks that referenced objects exist, is idempotent, and never
  re-emits (no ping-pong).
* **Recovery path.** Every time the link comes up, the sender first pushes a **full snapshot**: its
  whole state expressed as idempotent operations in dependency order (users, live sessions, groups,
  memberships and pending requests, files, seeders). The queue is cleared first, since the snapshot
  already contains its effects; operations that arrive during the snapshot stay queued and follow it.

### 2.3 Why this design

*Alternatives considered.* A primary/backup scheme would make one tracker a single point of failure
and give clients of the backup stale reads. Synchronous two-phase commit would block writes whenever the
peer is down, contradicting "keeps working with one tracker". Operation replication with eventual
consistency is the simplest scheme that meets every requirement: writes always succeed locally, and the
peer catches up.

*Why a snapshot instead of a log replay.* The trackers are in-memory. A restarted tracker has no log to
resume from, and a log kept long enough to cover any outage would grow without bound. A snapshot is
bounded by the size of the state and works identically for "peer restarted empty" and "two trackers
diverged in a partition".

*Why additive merging is enough.* Nearly all state is set-like (members, pending requests, seeders,
files): merging is set union, which is commutative and idempotent. So two trackers that each received a
different subset of operations converge once both have pushed a snapshot.

### 2.4 Sessions and liveness

Whether a user is *online* (and at what address) is not durable data: it is true only while a client
holds a connection. Therefore each tracker is authoritative only for sessions connected to it. A snapshot
contains only local sessions; on link failure, all sessions hosted by the peer are marked offline
immediately, so a tracker never hands out an address it can no longer vouch for. When the link returns,
the snapshot restores them.

### 2.5 Conflicts

The same username created on both trackers during a partition is resolved deterministically: the account
with the lower origin tracker number wins on both sides (tested: after healing, the "left" password works
and the "right" one is rejected on both trackers). Same-name **groups** are not resolved (see 6).

### 2.6 Client-side failover

A monitor thread peeks at the tracker socket every second. On failure it connects to the other tracker
and **logs the user in again** with credentials held in memory, so the user's address is registered and
others can still download from them, without the user typing anything. A request that meets a dead
tracker is retried once on the other. Requests and the monitor share one mutex, so their bytes never
interleave on the socket.

### 2.7 Overflow

The replicator queue is bounded at 10,000 operations. The first version dropped operations silently when
it was full, which could leave the trackers permanently different. Now an overflow sets a flag and the
sender performs a full resync.

## 3. Piece selection strategy

### 3.1 The problem in this system

A client becomes a seeder only after it holds the complete verified file. So every seeder has every
piece. The usual reason for **rarest-first** (keep scarce pieces alive when peers hold different subsets)
therefore does not arise: all pieces are equally available. What does matter is (a) using all peers'
bandwidth in parallel, (b) never asking a peer for something it cannot deliver, and (c) recovering from
bad or dead peers.

### 3.2 Algorithm

A shared *job board* of pending pieces (initially in index order), and up to four worker threads each
bound to one peer:

1. A worker takes the first pending piece its peer is eligible for, requests it, verifies the hash, and
   writes it at its offset.
2. On failure the piece returns to the board and a failure weight is recorded for *(piece, peer)*. A
   corrupt or refused answer fills the peer's whole allowance for that piece; a network error uses half
   of it (one retry, in case the drop was transient).
3. A peer that fails three pieces in a row is dropped for this download; its worker claims the next peer
   in the tracker's list that no worker has tried.
4. A worker with nothing eligible waits while other workers hold pieces (they may fail and return one)
   and exits when nothing is in flight.
5. All peer I/O has an 8-second timeout.

Sequential hand-out plus per-peer memory gives: no duplicate downloads in the normal case, automatic load
balancing (a fast peer simply takes more pieces), and correct handling of bad peers.

## 4. Protocol design rationale

* **One framing for all three protocols.** A fixed 14-byte header (magic, type, length, sequence)
  followed by a payload. One parser, one set of tests, and TCP's byte-stream nature is handled in one
  place: `recv_all`/`send_all` loop until exactly the requested number of bytes has moved.
* **Magic number and length bound.** A desynchronised stream (or a stranger connecting) is detected at
  the first header and the connection dropped; a length above 528,384 bytes is rejected, so a hostile
  length cannot make the tracker allocate gigabytes.
* **Sequence numbers.** The response echoes the request's number, which lets the replicator match acks
  and lets a downloader detect a reply that belongs to a different request.
* **Text for control, binary for data.** Control messages are `|`-separated fields: easy to read in a
  debugger and to extend. Piece data is raw bytes preceded by the 4-byte piece index the peer believes it
  is sending, so a mismatch is caught before the hash check.
* **Status codes** distinguish "you did something wrong" (bad request, not owner, not member) from "try
  elsewhere" (not found) and from server faults, so the client can react differently.
* **Trade-offs accepted:** host byte order in the header (simpler; assumes same-endian machines),
  no encryption (the assignment forbids external libraries), and `|` cannot appear in a token (the client
  rejects it).

## 5. Challenges encountered and how they were solved

These were found by deliberately stressing the finished system. Each led to a change and a regression
test.

**5.1 Throughput capped at 9 MiB/s on an idle machine.** A 64 MiB transfer took 6.9 s while the CPU was
almost idle: about 54 ms per piece. The cause was sending each message as two `send` calls (header, then
payload). With Nagle's algorithm the second small write waits for the first to be acknowledged, and the
receiver delays that acknowledgement (up to 40 ms) because it has nothing to reply with until the payload
arrives. Sending header and payload as a single write raised throughput to about 45 MiB/s (5x).
`TCP_NODELAY` on top made no measurable difference on loopback; it was kept as standard practice for a
request/response protocol.

**5.2 One corrupt peer made downloads fail.** With one honest and one corrupt seeder, every download
stopped at 78 of 80 pieces. Retries were counted per piece, and a corrupt peer fails in about a
millisecond, so it burned each piece's whole retry budget before the (slower) honest peer got a turn.
Failures are now recorded per *(piece, peer)*; a peer that gave bad data for a piece is never asked for
it again. After the fix the download succeeded in 5 of 5 rounds, and 3 of 3 in the end-to-end test.

**5.3 A frozen peer could hang the client.** Peer sockets had no timeouts, so a peer that accepted a
connection and never answered blocked a download thread forever, and because `logout` waits for downloads,
the whole client with it. All peer I/O now has an 8-second timeout, and a test uses a fake peer that
accepts and stays silent.

**5.4 Exit-time use-after-free (two places).** Both the client's seeder and the tracker's connection
handlers were detached threads holding pointers to state owned by `main`. Returning from `main` (`quit`)
destroyed that state while a thread could still be using it. Both now use the same pattern: stop
accepting, `shutdown()` live sockets to unblock threads stuck in `recv`, and wait on a condition variable
until the count of running threads reaches zero; only then is anything destroyed. A mutation test
(removing the wait) made ThreadSanitizer report races, proving the test can detect the bug.

**5.5 Multi-peer download depended on the first four peers.** Workers were bound to `peers[0..3]` and
gave up when their peer was dead, so a live fifth peer was never tried. Workers now claim peers from a
shared pool.

**5.6 Concurrent downloads.** The first version handled `download file` on the prompt thread, so only one
download ran at a time and `show downloads` had nothing to show. A `DownloadManager` runs each download
on its own thread and keeps a table of atomics for progress, so the prompt and the workers never wait for
each other.

**5.7 Weak password hashing.** The first version used an unsalted 64-bit FNV hash. It was replaced with a
per-user random salt and 5000 rounds of SHA-1, compared in constant time, with identical work for unknown
users (so timing does not reveal which accounts exist).

**5.8 Logout semantics.** The first version exited the client on `logout` and left uploaded files
registered on the tracker, so a returning user was advertised as a seeder of files their new process did
not hold. `logout` now withdraws every shared file, ends the session and keeps the client running. That
exposed a gap: a withdrawn file could never be shared again because its name was taken. An identical
re-upload by a non-seeder is now accepted as a re-share; different content under the same name is still
refused.

**5.9 Testing mistakes worth recording.** Two apparent failures during testing were errors in the test
scripts, not the system (background processes killed when a tool call returned, and a wait loop counting
distinct lines that could never reach its target). Each was confirmed as a test error before any code was
changed.

## 6. Limitations and future work

* **Piece sharing during download.** Only complete files are served. Serving verified pieces of
  in-progress downloads (with a "have" bitmap) would speed up flash crowds and make rarest-first
  meaningful.
* **Same-name group conflict.** Verified experimentally: if the same group name is created on both
  trackers during a partition, the owners differ after healing. The fix is a creation identity (origin
  plus counter) on each group and attributing later operations to it.
* **Authentication and confidentiality.** The sync channel is unauthenticated and nothing is encrypted.
* **Persistence.** Tracker state is in memory; if both trackers stop, it is lost.
* **End-game mode.** A frozen peer holding the last piece delays completion by its timeout; requesting
  the last pieces from several peers would remove that.
* **Owner leave, group deletion, ownership transfer** are not provided.

## 7. Evaluation

All numbers are from the final build, on a single-core 4 GB virtual machine with every process
(trackers and up to three clients) on one host.

**Correctness and robustness.** Unit tests (10 programs), seven integration scripts (13, 10, 9, 12, 15, 20
and 21 checks), the concurrency stress test (16 checks, three random seeds), the file-type test (32
checks), the failure-injection test (33 checks) and the valgrind sweep (19 checks) all pass. The code has
zero warnings under `-Wall -Wextra -Wpedantic -Wshadow`.

**Scale (1 GiB, 2048 pieces).** Every download was byte-identical to the original.

| Stage | Time | Peak client memory |
|---|---|---|
| Upload (hash the whole file) | 23 s | 6.3 MiB |
| Download from 1 seeder | 48 s | 6.7 MiB |
| Download from 2 seeders | 48 s | 7.6 MiB |

Memory is under 8 MiB for a 1024 MiB file, confirming the streaming design. Two seeders are not faster on
this machine because all clients share one CPU core and the work is dominated by SHA-1 verification, so
no multi-peer speed-up is claimed from these measurements.

**Throughput.** Before the fix in 5.1: about 9.3 MiB/s (single seeder, 64 MiB); after: about 45 MiB/s.

**Concurrency.** Three clients on different trackers create groups, exchange join requests, upload and
perform six cross-downloads simultaneously while issuing list commands; afterwards both trackers report
identical groups and files. Repeated with several random seeds.

**Failure injection.**

| Scenario | Outcome |
|---|---|
| One of two seeders killed mid-download | Download completes from the survivor, byte-identical |
| The only seeder killed mid-download | Reported as failed (`[F]`), client stays responsive and exits cleanly |
| A seeder serves corrupted data | Pieces re-fetched from the honest peer; file identical |
| A tracker killed during a transfer | Transfer finishes; the client re-announces itself as a seeder via the other tracker |
| Tracker-to-tracker network partition, then heal | Trackers diverge during the split, converge afterwards; account conflict resolved identically on both; cross-tracker download works after healing |
| A tracker frozen (`SIGSTOP`) then resumed | Client fails over in about 8 s; the resumed tracker catches up |

**Memory safety (valgrind, `--leak-check=full --track-fds=yes`).** The tracker under load, shut down with
`quit` while connections were still open, and two clients during multi-peer downloads (one quitting while
serving a peer) all show 0 errors, all heap blocks freed and no descriptors left open.

## 8. References

* Postel, J. (ed.). *Transmission Control Protocol*, RFC 793, 1981.
* Nagle, J. *Congestion Control in IP/TCP Internetworks*, RFC 896, 1984.
* Braden, R. (ed.). *Requirements for Internet Hosts: Communication Layers*, RFC 1122, 1989 (delayed
  acknowledgements, section 4.2.3.2).
* NIST. *Secure Hash Standard*, FIPS PUB 180-4, 2015 (SHA-1).
* Cohen, B. *The BitTorrent Protocol Specification* (BEP 3) and *Incentives Build Robustness in
  BitTorrent*, 2003 (piece-based transfer and integrity checking).
* Stevens, W. R., Fenner, B., Rudoff, A. M. *UNIX Network Programming, Volume 1*, Addison-Wesley, 2003.
* Linux manual pages: `socket(7)`, `tcp(7)`, `recv(2)`, `send(2)`, `shutdown(2)`, `setsockopt(2)`.
* Valgrind User Manual (Memcheck) and Clang/GCC ThreadSanitizer documentation.
* [Student: list here any other resource you actually consulted, and state any AI assistance used, as
  your course's policy requires.]
