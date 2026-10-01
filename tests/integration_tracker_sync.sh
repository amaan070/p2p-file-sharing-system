#!/bin/bash
# Integration test: live tracker-to-tracker sync.
# 2 trackers + 3 clients. alice and carol talk to TRACKER 2, bob talks to
# TRACKER 1, so every step only works if state crosses the sync link
# (in BOTH directions).
#
#   1. alice (T2): create user, group, upload A.bin           -> T2 -> T1
#   2. bob   (T1): create user, login, join group             -> T1 -> T2
#   3. alice (T2): accept bob                                 -> T2 -> T1
#   4. bob   (T1): list files + download A.bin from alice     (needs users,
#      group, membership, file meta and alice's address on T1)
#   5. bob becomes a seeder (add_seeder on T1 -> T2)
#   6. alice: stop share A.bin.  carol (T2) joins, downloads A.bin: only bob
#      can serve it -> needs stop_share + add_seeder + bob's login on T2.
#   7. kill tracker 2 with SIGKILL: tracker 1 must survive and keep working.
# Run from anywhere:  bash tests/integration_tracker_sync.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS=0; FAIL=0
T1_PID=""; T2_PID=""; ALICE_PID=""; BOB_PID=""

# Clients that should talk to tracker 2 use a copy with the lines swapped
# (a client tries line 1 first).
SWAPPED="$WORKDIR/tracker_info_swapped.txt"
grep -v '^#' "$INFO" | grep -v '^$' | tac > "$SWAPPED"

cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in $ALICE_PID $BOB_PID $T1_PID $T2_PID; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
wait_for() { for _ in $(seq 1 150); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }

head -c $((512*1024*4 + 321)) /dev/urandom > "$WORKDIR/A.bin"

"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/t1.log" 2>&1 < /dev/null & T1_PID=$!
"$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/t2.log" 2>&1 < /dev/null & T2_PID=$!
sleep 0.7

echo "=== Step 1: alice on TRACKER 2 creates user/group and uploads ==="
mkfifo "$WORKDIR/alice_fifo"
"$CLIENT_BIN" 127.0.0.1:8201 "$SWAPPED" < "$WORKDIR/alice_fifo" > "$WORKDIR/alice.out" 2>&1 & ALICE_PID=$!
exec 3>"$WORKDIR/alice_fifo"
grep -q "Connected to tracker at 127.0.0.1:9002" <(sleep 0.5; cat "$WORKDIR/alice.out") && ok "alice is attached to tracker 2" || bad "alice not attached to tracker 2"
printf 'create user alice pw1\nlogin alice pw1\ncreate group g1\nupload file g1 %s/A.bin\n' "$WORKDIR" >&3
sleep 1

echo "=== Step 2: bob on TRACKER 1 logs in with credentials created on tracker 2 ==="
mkfifo "$WORKDIR/bob_fifo"
"$CLIENT_BIN" 127.0.0.1:8202 "$INFO" < "$WORKDIR/bob_fifo" > "$WORKDIR/bob.out" 2>&1 & BOB_PID=$!
exec 4>"$WORKDIR/bob_fifo"
grep -q "Connected to tracker at 127.0.0.1:9001" <(sleep 0.5; cat "$WORKDIR/bob.out") && ok "bob is attached to tracker 1" || bad "bob not attached to tracker 1"
printf 'create user bob pw2\nlogin bob pw2\njoin group g1\n' >&4
sleep 1
# 'join group g1' only succeeds if group g1 (created on T2) exists on T1.
[ "$(grep -c '^OK' "$WORKDIR/bob.out")" -ge 3 ] && ok "bob: create user / login / join group (group replicated T2->T1) all OK" || { bad "bob's commands did not all succeed"; cat "$WORKDIR/bob.out"; }

echo "=== Step 3: alice (T2) accepts bob's request that was made on T1 ==="
printf 'list requests g1\naccept request g1 bob\n' >&3
sleep 1
grep -q "OK: bob" "$WORKDIR/alice.out" && ok "T2 saw bob's join request (replicated T1->T2)" || bad "alice did not see bob's join request"

echo "=== Step 4: bob (T1) lists and downloads from alice ==="
printf 'list files g1\ndownload file g1 A.bin %s/bob_A.bin\n' "$WORKDIR" >&4
wait_for "$WORKDIR/bob.out" "\[C\] \[g1\] A.bin" && ok "bob downloaded A.bin using metadata + alice's address replicated to T1" || { bad "bob could not download A.bin"; tail -5 "$WORKDIR/bob.out"; }
[ "$(sha1sum < "$WORKDIR/A.bin")" = "$(sha1sum < "$WORKDIR/bob_A.bin" 2>/dev/null)" ] && ok "bob's copy SHA1 matches" || bad "bob's copy differs"
sleep 0.5

echo "=== Step 6: alice stops sharing; carol (T2) downloads -> only bob can serve ==="
printf 'stop share g1 A.bin\n' >&3
sleep 0.5
printf 'create user carol pw3\nlogin carol pw3\njoin group g1\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8299 "$SWAPPED" > /dev/null 2>&1
printf 'accept request g1 carol\n' >&3
sleep 0.7
printf 'login carol pw3\ndownload file g1 A.bin %s/carol_A.bin\nlogout\n' "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:8203 "$SWAPPED" > "$WORKDIR/carol.out" 2>&1
grep -q "\[C\] \[g1\] A.bin" "$WORKDIR/carol.out" && ok "carol (T2) downloaded from bob, who registered as seeder on T1" || { bad "carol could not download"; cat "$WORKDIR/carol.out" | tail -5; }
[ "$(sha1sum < "$WORKDIR/A.bin")" = "$(sha1sum < "$WORKDIR/carol_A.bin" 2>/dev/null)" ] && ok "carol's copy SHA1 matches" || bad "carol's copy differs"

echo "=== Step 7: tracker 2 is killed (SIGKILL); tracker 1 must survive ==="
grep -q "\[sync\] connected to peer" "$WORKDIR/t1.log" && grep -q "\[sync\] connected to peer" "$WORKDIR/t2.log" && ok "both trackers logged an established sync link" || bad "sync link log line missing"
kill -9 "$T2_PID"; wait "$T2_PID" 2>/dev/null; T2_PID=""
sleep 0.5
printf 'create user dave pw4\nlogin dave pw4\ncreate group g2\ncreate group g3\nlist groups\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8204 "$INFO" > "$WORKDIR/dave.out" 2>&1
grep -q "g1|g2|g3" "$WORKDIR/dave.out" && ok "tracker 1 kept serving clients after its peer died" || { bad "tracker 1 stopped working"; tail -5 "$WORKDIR/dave.out"; }
kill -0 "$T1_PID" 2>/dev/null && ok "tracker 1 process still alive (no SIGPIPE crash)" || bad "tracker 1 died"
grep -qE "unreachable|lost connection" "$WORKDIR/t1.log" && ok "tracker 1 logged that the peer is gone" || bad "no peer-down log line"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
