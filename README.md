# Peer-to-Peer Distributed File Sharing System

A group-based file sharing system in C++17 using only POSIX sockets, `std::thread` and the standard
library. Two synchronised **trackers** keep the metadata (users, groups, files, who is seeding what);
the file bytes move **directly between clients**, in 512 KB pieces, from several peers at once, with
every piece verified by SHA-1.

Contents: [1. Build](#1-build) · [2. Run](#2-run) · [3. Commands](#3-commands) ·
[4. Architecture](#4-architecture) · [5. Key algorithms](#5-key-algorithms) ·
[6. Data structures](#6-data-structures-and-why) · [7. Network protocol](#7-network-protocol) ·
[8. Assumptions](#8-assumptions) · [9. Features and limitations](#9-features-and-limitations) ·
[10. Testing](#10-testing)

---

## 1. Build

Requirements: Linux, `g++` with C++17, `make`. No third-party libraries.

```bash
make            # builds tracker/tracker and client/client
make tracker    # only the tracker
make client     # only the client
make test       # builds and runs every unit test
make clean      # removes all build products
```

`tracker/` and `client/` also have their own Makefiles (`cd tracker && make`). Both include the small
shared library in `common/`, which therefore must stay next to them. The project is compiled with `-Wall -Wextra -Wpedantic -Wshadow`.

The code uses no `system()`, `exec*` or `popen()`, and no filesystem, torrent or database library (only
the `stat()` system call, to tell a directory from a file).

## 2. Run

### `tracker_info.txt`

One `IP:PORT` per line; **line 1 is tracker 1, line 2 is tracker 2**. Blank lines and `#` comments are
ignored; Windows line endings are tolerated.

```
127.0.0.1:9001
127.0.0.1:9002
```

### Start the trackers (two terminals)

```bash
./tracker/tracker tracker_info.txt 1
./tracker/tracker tracker_info.txt 2
```

`tracker_no` selects which line of the file is this tracker's own address; the other line is its sync
peer. Type `quit` at a tracker's console to stop it (`SIGINT`/`SIGTERM` also stop it cleanly). The
system keeps working while at least one tracker is up; a tracker started later, or restarted with empty
state, is refilled automatically from the other.

### Start clients (one terminal each)

```bash
./client/client 127.0.0.1:7001 tracker_info.txt
```

`<IP>:<PORT>` is the address **this client listens on** to serve pieces to other peers, so every client
needs its own port. The client tries the trackers in file order and fails over automatically.

## 3. Commands

| Command | What it does |
|---|---|
| `create user <user_id> <password>` | Register an account |
| `login <user_id> <password>` | Start a session (this client becomes a peer others can download from) |
| `create group <group_id>` | Create a group; you become its owner |
| `join group <group_id>` | Ask to join |
| `leave group <group_id>` | Leave (the owner cannot leave, see limitations) |
| `list groups` | All groups in the system |
| `list requests <group_id>` | Pending join requests (owner only) |
| `accept request <group_id> <user_id>` | Accept a request (owner only) |
| `upload file <group_id> <file_path>` | Share a local file with a group |
| `list files <group_id>` | Files shared in the group (members only) |
| `download file <group_id> <file_name> <destination>` | Download in the background. `<destination>` is the path of the file to create, **or an existing directory** (saved as `<dir>/<file_name>`) |
| `show downloads` | One line per download: `[C] [group] file` complete, `[D] [group] file (12/49 pieces, 24%)` running, `[F] [group] file (failed)` |
| `stop share <group_id> <file_name>` | Stop sharing a file |
| `logout` | End the session **and stop sharing all files** (withdraws them from the tracker). The client keeps running, so you can log in again |
| `quit` | Same as logout, then exit the client |

Replies are `OK`, `OK: <items separated by '|'>`, or `ERROR (code N)` where N is a status code
(section 7.3). Downloads run on their own threads, so several can be in progress at once and the prompt
stays usable; `logout` and `quit` wait for running downloads to finish first.

Arguments must not contain `|` (the protocol's field separator) or whitespace; the client refuses them
with a message. The shared file name is the base name of the uploaded path.

## 4. Architecture

```
      Tracker 1  <==== tracker sync (TCP, 5.2) ====>  Tracker 2
          ^                                              ^
          |  control: users, groups, file metadata,      |
          |  who seeds what (TCP, 7.1)                   |
          +----------+-------------+---------------------+
                     |             |
                 Client A <---> Client B <---> Client C
                     piece transfer, client to client
                     (TCP, 7.2): the actual file bytes
```

Three TCP protocols share one framing format: **client-tracker** (metadata), **client-client**
(pieces), **tracker-tracker** (replication).

### Source layout

| Path | Contents |
|---|---|
| `common/protocol.h` | Wire header, message types, status codes, `PIECE_SIZE` |
| `common/network_utils.*` | Reliable framed send/receive (loops over partial I/O), field split/join, `TCP_NODELAY` |
| `common/sha1.*` | SHA-1 implemented from scratch (streaming; checked against known vectors) |
| `common/file_chunker.*` | Piece arithmetic, streaming per-piece and whole-file hashing, read/write a piece at an offset |
| `common/tracker_info.*` | Parses `tracker_info.txt` and `IP:PORT` |
| `tracker/tracker_state.*` | All tracker data and operations behind one `shared_mutex`; also the replication "apply" side |
| `tracker/connection_handler.*` | One thread per connection; request dispatch; `ConnectionSet` for clean shutdown |
| `tracker/replicator.*` | Tracker-to-tracker sync sender: queue, heartbeat, reconnect, snapshot |
| `tracker/main.cpp` | Listener, console (`quit`), signals, orderly shutdown |
| `client/tracker_client.*` | Talks to the trackers; failover; monitor thread; automatic re-login |
| `client/downloader.*` | Multi-peer downloader (work queue, per-peer failure tracking, verification) |
| `client/download_manager.*` | Runs several downloads at once; the table behind `show downloads` |
| `client/seeder.*` | Listener and handler threads serving pieces; `stop_all_seeders()` |
| `client/file_registry.*` | Which local files this client is sharing (thread-safe map) |
| `client/main.cpp` | Command loop |
| `tests/` | Unit tests, integration/stress/failure scripts, `tcp_proxy` (partition injection) |

### Threads

* **Tracker:** main (accept), console, one handler per connection, one replicator sender.
* **Client:** main (prompt), tracker monitor, seeder accept loop plus one handler per connected peer,
  and one thread per active download with up to 4 piece workers inside it.

## 5. Key algorithms

### 5.1 File integrity and streaming

A file is split into 512 KB pieces (the last may be smaller; a zero-byte file is one empty piece). On
upload the client reads the file **once**, feeding each piece to a per-piece SHA-1 and the same bytes to
a whole-file SHA-1; the tracker stores all hashes. Nothing larger than one piece is held in memory. Each
received piece is hashed immediately and compared with the tracker's hash; a mismatch discards it. After
the last piece the whole file is hashed again and compared with the file hash. Pieces are written at
`index x 512 KB` in a pre-sized file, so they can arrive in any order.

### 5.2 Tracker synchronisation

*Model:* operation-based replication with eventual consistency.

1. Every state-changing operation that **succeeds locally** (create user, login, logout, create/join/
   leave group, accept, upload, stop share, add seeder) is also emitted, still under the state lock so
   order is preserved, to the **replicator**, which queues it.
2. The replicator thread keeps one persistent TCP connection to the peer tracker and sends queued
   operations one at a time, waiting for a `SYNC_ACK` with the same sequence number. When idle it sends a
   `SYNC_HEARTBEAT` every second, so a dead peer is noticed within a few seconds.
3. The receiving tracker **applies** the operation without re-validating business rules (the origin
   already did; re-checking would cause false rejections from ordering differences). Applying is
   **idempotent** and **additive** (set insertions, "create if missing") and never re-emits, so there is
   no ping-pong.
4. **Recovery:** whenever the link is (re)established, the sender first pushes a **full snapshot** of its
   state as idempotent operations in dependency order (users, live sessions, groups, members and
   requests, files, seeders). This refills a tracker restarted empty, and merges two trackers that
   diverged during a partition (both directions push, so the result is the union).
5. **Sessions are local:** each tracker is authoritative only for sessions connected to *it*. Snapshots
   carry only local sessions, and when the link drops the sessions hosted by the peer are marked offline
   so their possibly dead addresses stop being handed out as seeders.
6. **Account conflicts:** the same username created independently on both trackers (for example during a
   partition) resolves deterministically; the account from the **lower-numbered tracker** wins on both.
7. **Queue overflow:** the queue is bounded (10,000 operations). If it overflows, the overflow is
   recorded and repaired with a full resync, so the trackers cannot silently diverge.

*Client side of failover:* a monitor thread notices a dead tracker connection even while the user is
idle, switches to the other tracker and **logs the user back in automatically** (credentials kept in
memory only). A request that hits a dead tracker is retried once on the other.

### 5.3 Multi-peer download and piece selection

The tracker returns the current seeders of the file. The downloader runs up to **4 worker threads**, each
bound to one peer, over a shared job board of pieces:

* A worker takes the next piece its peer is **eligible** for, requests it, verifies the SHA-1 and writes
  it at its offset.
* On any failure the piece goes back on the board and the failure is **remembered per (piece, peer)**. A
  peer that returned corrupt data (or refused) for a piece is never asked for that piece again, so it is
  re-fetched from a *different* peer as the spec requires. A dropped connection is treated as possibly
  transient and gets one retry before that peer is excluded for that piece.
* A peer that fails **3 pieces in a row** is dropped from the download, and its worker claims the next
  unused peer from the tracker's list (so a live peer at the end of a long list is still found).
* A worker with nothing eligible **waits** while other workers still hold pieces, because one of them
  may fail and put back a piece only this worker can serve.
* Every peer socket has an 8-second timeout, so a frozen peer cannot block a download forever.

**Why not rarest-first?** Rarest-first spreads scarce pieces around a swarm of partial downloaders. Here
a client becomes a seeder only after it holds the *complete, verified* file, so every seeder has every
piece and all pieces are equally rare. Pieces are therefore handed out in index order; several peers
give parallel bandwidth and fault tolerance, not rarity balancing (see limitation 1).

### 5.4 Concurrent downloads

`DownloadManager` keeps a table (one row per download) and starts one thread per download; `download
file` returns immediately. Progress counters are atomics written by the workers and read by `show
downloads`, so neither side waits for the other. Duplicate downloads of the same file, two downloads into
the same destination, and more than 16 at once are refused. When a download completes, the client
registers the file locally **first**, then tells the tracker it is now a seeder (otherwise a peer could be
sent to it before it can serve the file).

### 5.5 Clean shutdown

Detached handler threads hold pointers to shared state, so both programs join them before exiting:
`stop_all_seeders()` (client) and `ConnectionSet::shutdown_and_wait()` (tracker) stop accepting, call
`shutdown()` on live sockets to unblock threads stuck in `recv`, and wait until every thread has left.
Only then is the state destroyed. This was validated with valgrind, AddressSanitizer and
ThreadSanitizer.

### 5.6 Passwords

Stored as `<salt>$<hash>`: a random 16-byte per-user salt and SHA-1 iterated 5000 times over
(salt + password), compared in constant time; an unknown user costs the same time as a wrong password.
Only this record is stored or replicated, never the plaintext. It is weaker than bcrypt or Argon2, but
those are external libraries the assignment does not allow.

## 6. Data structures and why

| Structure | Where | Why |
|---|---|---|
| `std::map` for users, groups and per-group files | `TrackerState` | Ordered, so listings and replication snapshots are deterministic; O(log n) is ample at this scale |
| `std::set<string>` for members, pending requests, seeders | `Group`, `FileMeta` | Uniqueness for free, and re-inserting is a no-op, which makes replayed replication operations harmless |
| `std::shared_mutex` | `TrackerState` | Many concurrent readers (list, get-info), exclusive writers |
| Bounded `std::deque<string>` + mutex + condvar | `Replicator` | FIFO of already-serialised operations, so the state lock is held only briefly |
| `deque<piece>` + `vector<map<peer, weight>>` + in-flight count + condvar | `SharedDownloadState` | The job board: pending pieces, who failed what, and when a waiting worker may give up |
| `vector<unique_ptr<Entry>>` with atomic counters | `DownloadManager` | Stable row addresses for threads; lock-free progress updates |
| `unordered_map<"group\x1ffile", path>` + mutex | `FileRegistry` | O(1) lookup on every piece request; `\x1f` cannot occur in a token |
| `set<fd>` + active count + condvar | `SeederState`, `ConnectionSet` | Lets shutdown unblock and wait for every handler thread |

## 7. Network protocol

### 7.1 Framing (all three protocols)

Every message is a fixed 14-byte header followed by `payload_len` bytes:

| Field | Size | Meaning |
|---|---|---|
| `magic` | 4 | `0x50325053` ("P2PS"); anything else means the stream is desynchronised and the connection is dropped |
| `type` | 2 | Message type (7.2) |
| `payload_len` | 4 | Bytes that follow; must be at most 528,384 (512 KB + 4 KB), otherwise the connection is dropped |
| `seq_no` | 4 | Request/response correlation; the reply echoes it |

`send_all`/`recv_all` loop until the exact byte count is transferred, so partial reads and writes and
message coalescing on the TCP stream are handled. A header and its payload are sent as **one** write (two
small writes triggered Nagle/delayed-ACK stalls that capped throughput at about 9 MiB/s; see the report).
Control payloads are text fields separated by `|`; piece data is binary.

### 7.2 Messages

Requests need a logged-in session except `CREATE_USER`, `LOGIN` and `LIST_GROUPS`. Replies are
`RESP_OK` or `RESP_ERR` with payload `status[|fields...]`.

| Type (value) | Request payload | Success reply fields |
|---|---|---|
| `CREATE_USER` (1) | `user\|password` | none |
| `LOGIN` (2) | `user\|password\|listen_ip\|listen_port` | none |
| `LOGOUT` (3) | none | none |
| `CREATE_GROUP` (4), `JOIN_GROUP` (5), `LEAVE_GROUP` (6) | `group` | none |
| `LIST_GROUPS` (7) | none | `group\|group\|...` |
| `LIST_REQUESTS` (8) | `group` | `user\|user\|...` |
| `ACCEPT_REQUEST` (9) | `group\|user` | none |
| `UPLOAD_FILE` (20) | `group\|name\|size\|full_sha1\|piece_sha1_0\|...` | none |
| `LIST_FILES` (21) | `group` | `name\|name\|...` |
| `DOWNLOAD_FILE` (22) | `group\|name` | `size\|full_sha1\|n\|hash_0..hash_n-1\|m\|ip:port_0..ip:port_m-1` |
| `STOP_SHARE` (23) | `group\|name` | none |
| `ADD_SEEDER` (24) | `group\|name` | none (sent when a download finishes) |
| `PIECE_REQUEST` (41) | `group\|name\|piece_index` | (client to client) |
| `PIECE_DATA` (42) | none | 4-byte big-endian piece index + raw piece bytes |
| `SYNC_OP` (60) | operation fields (below) | `SYNC_ACK` (61), same `seq_no` |
| `SYNC_HEARTBEAT` (62) | none | `SYNC_ACK` |

Replication operations (`SYNC_OP` fields): `create_user|id|pwd_record|origin_tracker`,
`login|id|ip|port`, `logout|id`, `create_group|group|owner`, `request_join|group|user`,
`accept_request|group|user`, `leave_group|group|user`,
`upload_file|group|uploader|name|size|full_hash|piece_hashes...`, `stop_share|group|user|name`,
`add_seeder|group|user|name`.

### 7.3 Status codes

`0` OK, `1` unknown, `2` bad request, `3` authentication failed, `4` not logged in, `5` already exists,
`6` not found, `7` not the owner, `8` not a member, `9` already a member/requested, `10` internal error.

### 7.4 Timeouts

Client to tracker: connect/send 3 s, receive 8 s (a timeout is treated as a dead tracker and triggers
failover). Tracker to peer tracker: 2 s send, 3 s receive; heartbeat and reconnect every 1 s. Client to
peer: 8 s per operation. The client checks its tracker connection every second.

## 8. Assumptions

* A trusted network: the protocols are plain TCP without encryption, and the sync channel is
  unauthenticated, so passwords travel unencrypted.
* All machines use the same byte order for the 14-byte header (little-endian x86). Piece indices inside
  `PIECE_DATA` are big-endian.
* `tracker_info.txt` lists exactly two trackers; line order defines their identity.
* Each client uses its own listen port, reachable by the other clients.
* Tokens (user, group and file names, passwords) contain no spaces and no `|`.
* Files up to 1 GB were the target. The payload cap allows roughly 12,000 pieces (about 6 GB) before one
  file's hash list no longer fits in a single message; sizes up to 1 GiB were tested.
* Tracker state lives in memory. A restarted tracker is refilled from its peer; if both trackers are
  stopped, the state is lost.

## 9. Features and limitations

**Implemented:** every command in section 3; 512 KB pieces with per-piece and whole-file SHA-1;
streaming hashing and transfer (constant memory); multi-peer, multi-file concurrent downloads with retry
from a different peer and timeouts; automatic seeding after download; two-tracker replication with
automatic recovery and resync; client failover with automatic re-login; graceful tracker and client
shutdown with no leaks; salted iterated password hashing; input validation.

**Known limitations:**

1. **Pieces are shared only after the whole file is verified**, not while it is downloading, so a client
   cannot serve pieces of a file it is still fetching.
2. **Same group name created on both trackers during a partition:** the trackers do not converge on the
   owner (users do converge via the tie-break). A fix needs a per-group creation identity so later
   operations are attributed to the right incarnation.
3. **The group owner cannot leave** their group; there is no group deletion or ownership transfer.
4. **The sync channel is unauthenticated:** any process that can reach a tracker could send `SYNC_OP`.
5. **A frozen peer holding the last piece** delays completion by up to its 8 s timeout (no "end-game"
   duplicate requests).
6. Retrying a request after a mid-request tracker failure can report "already exists" if the first
   attempt had in fact been applied (at-least-once retry).
7. `list groups` does not require a login. File and user names cannot contain spaces.

## 10. Testing

Run everything from the project root after `make`. Integration scripts start their own trackers and
clients on `127.0.0.1` (ports 8000-9800) and clean up after themselves. `valgrind` and GNU `time`
(`/usr/bin/time`) must be installed for the last two scripts.

### Unit tests: `make test`

Protocol framing, SHA-1, piece chunking, tracker state, file management, replication (apply, snapshot,
conflicts), password storage, seeder shutdown, and the single- and multi-peer downloader (including
corrupt seeders, an all-corrupt swarm and a frozen peer).

### Integration and stress scripts (`bash tests/<name>.sh`)

| Script | What it proves |
|---|---|
| `integration_upload_download.sh` | Upload/download across size boundaries, byte for byte |
| `integration_sessions.sh` | `logout` keeps the client alive, withdraws files, allows re-login; directory destinations |
| `integration_seeding.sh` | A finished download makes the client a seeder; multi-peer path used |
| `integration_tracker_sync.sh` | Actions on one tracker are visible on the other, including cross-tracker downloads |
| `integration_resync.sh` | A tracker killed and restarted empty is refilled from its peer |
| `integration_failover.sh` | Rolling tracker crashes with long-running clients; concurrent load |
| `integration_concurrent_downloads.sh` | Several downloads at once, `show downloads`, duplicate refusal, `logout` waits |
| `stress_concurrency.sh` | 3 clients and 2 trackers, overlapping group management, uploads and six cross-downloads; trackers must end identical |
| `file_types.sh` | Text, ELF executable, PNG, tar.gz, zip, empty file, 1 byte, 512 KB +/- 1, delimiter and NUL bytes |
| `failure_injection.sh` | Seeder killed mid-download; only seeder killed; corrupt seeder; tracker killed mid-transfer; **tracker-to-tracker network partition and heal** (via `tcp_proxy`); frozen tracker (`SIGSTOP`) |
| `stress_large_file.sh [bytes]` | A 1 GiB file: memory (GNU `time`), correctness, multi-peer |
| `valgrind_sweep.sh` | Tracker under load (connections open at `quit`) and clients during multi-peer downloads, with `--leak-check=full --track-fds=yes` |

### Manual check

```bash
./tracker/tracker tracker_info.txt 1 &   ./tracker/tracker tracker_info.txt 2 &
./client/client 127.0.0.1:7001 tracker_info.txt      # terminal A
./client/client 127.0.0.1:7002 tracker_info.txt      # terminal B
# A: create user a pw / login a pw / create group g / upload file g /path/file
# B: create user b pw / login b pw / join group g          A: accept request g b
# B: download file g file /tmp/   then   show downloads   ->  [C] [g] file
```

### Results (final build, single-core 4 GB VM)

All numbers below are from the final build.

| Check | Result |
|---|---|
| Compiler warnings (`-Wall -Wextra -Wpedantic -Wshadow`) | 0 |
| Unit tests (`make test`, 10 programs) | all pass |
| Integration scripts (upload/download, sessions, seeding, tracker sync, resync, failover, concurrent downloads) | 13, 10, 9, 12, 15, 20, 21 checks, all pass |
| `stress_concurrency.sh` (3 clients + 2 trackers, three random seeds) | 16 of 16 in each run; both trackers end identical |
| `file_types.sh` | 32 of 32 |
| `failure_injection.sh` (6 scenarios incl. partition and heal) | 33 of 33 |
| `valgrind_sweep.sh` (tracker under load; two clients) | 19 of 19: 0 errors, no leaks, no leaked descriptors |
| `stress_large_file.sh` (1 GiB, 2048 pieces) | 10 of 10; every download byte-identical |

**1 GiB file** (peak resident memory of each client, measured with GNU `time`):

| Stage | Time | Peak memory |
|---|---|---|
| Upload (hash the whole file) | 23 s | 6.3 MiB |
| Download from 1 seeder | 48 s | 6.7 MiB |
| Download from 2 seeders | 48 s | 7.6 MiB |

Peak memory stays under 8 MiB for a 1024 MiB file, confirming that data is streamed piece by piece. The
two-seeder download is not faster here only because this VM has a single CPU core shared by all three
clients and the run is CPU-bound (SHA-1 verification); on separate machines the extra seeder adds
bandwidth.
