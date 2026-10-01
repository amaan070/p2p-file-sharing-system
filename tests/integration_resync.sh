#!/bin/bash
# Integration test: reconnect + full-state resync.
#
#   1. T1, T2 up. alice (on T2) creates group g1 and uploads A.bin; bob and
#      carol (on T1) join; bob downloads A.bin and becomes a seeder.
#   2. T2 is killed (SIGKILL = crash, all its state is lost).
#   3. T1 must notice (heartbeat) and mark alice -- whose session lived on
#      T2 -- offline: bob asking for A.bin again gets no peers (alice's
#      process is still running, so a STALE online flag would let it succeed).
#      alice's client is given a tracker list whose second entry is
#      unreachable, so it CANNOT fail over (clients that can are covered by
#      the failover test) -- her session really is gone.
#   4. While T2 is down, changes are made on T1 (user erin, group g2).
#   5. T2 is restarted with EMPTY state. T1 reconnects to it and pushes a
#      snapshot. T2 must now know every user/group/file, including the
#      changes made during the outage.
#   6. carol logs in on T2 and downloads A.bin: needs recovered accounts,
#      membership, file metadata AND bob's live session/seeder entry.
#   7. A change made on the restarted T2 replicates back to T1.
# Run from anywhere:  bash tests/integration_resync.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS=0; FAIL=0
T1_PID=""; T2_PID=""; ALICE_PID=""; BOB_PID=""

SWAPPED="$WORKDIR/tracker_info_swapped.txt"   # clients using this attach to tracker 2
grep -v '^#' "$INFO" | grep -v '^$' | tac > "$SWAPPED"
# T2 first, then an address nobody listens on: alice's client has nowhere to fail over to.
ALICE_INFO="$WORKDIR/tracker_info_alice.txt"
printf '127.0.0.1:9002\n127.0.0.1:9999\n' > "$ALICE_INFO"

cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in $ALICE_PID $BOB_PID $T1_PID $T2_PID; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
wait_for() { for _ in $(seq 1 ${3:-150}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
wait_count() { for _ in $(seq 1 ${3:-200}); do [ "$(grep -c -- "$2" "$1" 2>/dev/null)" -ge "$4" ] && return 0; sleep 0.1; done; return 1; }
run_client() { # run_client <port> <info> <outfile>  (commands on stdin)
    "$CLIENT_BIN" 127.0.0.1:"$1" "$2" > "$3" 2>&1
}

head -c $((512*1024*3 + 999)) /dev/urandom > "$WORKDIR/A.bin"

"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/t1.log" 2>&1 < /dev/null & T1_PID=$!
"$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/t2.log" 2>&1 < /dev/null & T2_PID=$!
wait_for "$WORKDIR/t1.log" "connected to peer" 60 && wait_for "$WORKDIR/t2.log" "connected to peer" 60 \
    && ok "trackers connected to each other at startup (no client traffic needed)" || bad "trackers did not connect at startup"

echo "=== Step 1: normal operation ==="
mkfifo "$WORKDIR/alice_fifo" "$WORKDIR/bob_fifo"
"$CLIENT_BIN" 127.0.0.1:8301 "$ALICE_INFO" < "$WORKDIR/alice_fifo" > "$WORKDIR/alice.out" 2>&1 & ALICE_PID=$!
exec 3>"$WORKDIR/alice_fifo"
printf 'create user alice pw1\nlogin alice pw1\ncreate group g1\nupload file g1 %s/A.bin\n' "$WORKDIR" >&3
sleep 1
"$CLIENT_BIN" 127.0.0.1:8302 "$INFO" < "$WORKDIR/bob_fifo" > "$WORKDIR/bob.out" 2>&1 & BOB_PID=$!
exec 4>"$WORKDIR/bob_fifo"
printf 'create user bob pw2\nlogin bob pw2\njoin group g1\n' >&4
printf 'create user carol pw3\nlogin carol pw3\njoin group g1\nlogout\n' | run_client 8399 "$INFO" /dev/null
sleep 0.5
printf 'accept request g1 bob\naccept request g1 carol\n' >&3
sleep 1
printf 'download file g1 A.bin %s/bob_A.bin\n' "$WORKDIR" >&4
wait_for "$WORKDIR/bob.out" "\[C\] \[g1\] A.bin" && ok "bob downloaded A.bin and is now a seeder (session on T1)" || bad "bob's first download failed"

echo "=== Step 2/3: T2 crashes (kill -9); T1 must notice and mark alice offline ==="
kill -9 "$T2_PID"; wait "$T2_PID" 2>/dev/null; T2_PID=""
wait_for "$WORKDIR/t1.log" "lost connection to peer" 60 && ok "T1 detected the dead peer (heartbeat) without any client traffic" || bad "T1 did not detect the crash"
sleep 1.5
printf 'download file g1 A.bin %s/bob_A_again.bin\n' "$WORKDIR" >&4
sleep 3
if [ "$(grep -c '\[C\] \[g1\] A.bin' "$WORKDIR/bob.out")" -eq 1 ] && [ ! -s "$WORKDIR/bob_A_again.bin" ]; then
    ok "alice (session was on the dead tracker) is no longer advertised as a seeder, though her process still runs"
else
    bad "stale online flag: alice still advertised after T2 crashed"; tail -4 "$WORKDIR/bob.out"
fi

echo "=== Step 4: changes on T1 while T2 is down ==="
printf 'create user erin pw5\nlogin erin pw5\ncreate group g2\nlogout\n' | run_client 8398 "$INFO" "$WORKDIR/erin.out"
grep -c '^OK' "$WORKDIR/erin.out" | grep -q '^4$' && ok "T1 kept accepting changes with its peer down" || { bad "T1 rejected changes while peer was down"; cat "$WORKDIR/erin.out"; }

echo "=== Step 5: T2 restarts with EMPTY state ==="
"$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/t2b.log" 2>&1 < /dev/null & T2_PID=$!
wait_for "$WORKDIR/t2b.log" "connected to peer" 100 && ok "restarted T2 connected to T1" || bad "restarted T2 never connected"
wait_count "$WORKDIR/t1.log" "resync complete" 200 2 && ok "T1 pushed a full snapshot to the restarted T2" || bad "T1 never resynced T2"
sleep 0.7

echo "=== Step 6: everything is back on T2 ==="
printf 'login carol pw3\nlist groups\nlist files g1\ndownload file g1 A.bin %s/carol_A.bin\nlogout\n' "$WORKDIR" | run_client 8303 "$SWAPPED" "$WORKDIR/carol.out"
grep -q "Connected to tracker at 127.0.0.1:9002" "$WORKDIR/carol.out" && ok "carol is attached to the restarted T2" || bad "carol not on T2"
grep -q "g1" "$WORKDIR/carol.out" && grep -q "g2" "$WORKDIR/carol.out" && ok "T2 knows group g1 and g2 (g2 was created during the outage)" || { bad "groups missing on T2"; cat "$WORKDIR/carol.out"; }
grep -q "A.bin" "$WORKDIR/carol.out" && ok "T2 knows file A.bin" || bad "file missing on T2"
grep -q "\[C\] \[g1\] A.bin" "$WORKDIR/carol.out" && ok "carol (account+membership recovered) downloaded from bob (session+seeder recovered)" || { bad "carol's download failed"; tail -5 "$WORKDIR/carol.out"; }
[ "$(sha1sum < "$WORKDIR/A.bin")" = "$(sha1sum < "$WORKDIR/carol_A.bin" 2>/dev/null)" ] && ok "carol's copy SHA1 matches" || bad "carol's copy differs"
printf 'login erin pw5\nlogout\n' | run_client 8304 "$SWAPPED" "$WORKDIR/erin2.out"
grep -c '^OK' "$WORKDIR/erin2.out" | grep -q '^2$' && ok "account created during the outage (erin) can log in on T2" || bad "erin cannot log in on T2"

echo "=== Step 7: changes made on restarted T2 replicate back to T1 ==="
printf 'login erin pw5\ncreate group g4\nlogout\n' | run_client 8305 "$SWAPPED" /dev/null
sleep 0.8
printf 'list groups\n' >&4
sleep 0.8
grep -q "g4" "$WORKDIR/bob.out" && ok "group g4 (created on T2 after restart) is visible on T1" || bad "T2->T1 replication broken after restart"

echo "=== Health ==="
kill -0 "$T1_PID" 2>/dev/null && kill -0 "$T2_PID" 2>/dev/null && ok "both trackers alive at the end" || bad "a tracker died"
echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
