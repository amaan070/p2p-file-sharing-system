#!/bin/bash
# Integration test: a client that finishes a download becomes
# a seeder, and other clients can then download from it.
#
# Scenario (1 tracker, 3 clients):
#   1. alice uploads A.bin and B.bin, stays online (seeder).
#   2. bob downloads both from alice, then stays online. Bob is now a seeder.
#   3. alice runs "stop share" on B.bin (alice no longer serves B).
#   4. carol downloads A.bin  -> alice + bob both seed it (multi-peer path).
#   5. carol downloads B.bin  -> ONLY bob can serve it. Success proves that
#      bob was registered as a seeder and that alice's stop share worked.
# Run from anywhere:  bash tests/integration_seeding.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
TRACKER_INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS=0; FAIL=0
TRACKER_PID=""; ALICE_PID=""; BOB_PID=""

cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in $ALICE_PID $BOB_PID $TRACKER_PID; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# Wait (up to ~15s) until $2 appears in file $1.
wait_for() {
    for _ in $(seq 1 150); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}

PIECE=$((512*1024))
head -c $((PIECE*5 + 777)) /dev/urandom > "$WORKDIR/A.bin"
head -c $((PIECE*3 + 55))  /dev/urandom > "$WORKDIR/B.bin"

"$TRACKER_BIN" "$TRACKER_INFO" 1 > "$WORKDIR/tracker.log" 2>&1 < /dev/null &
TRACKER_PID=$!
sleep 0.5

# --- alice: uploader, kept alive through a FIFO ---
mkfifo "$WORKDIR/alice_fifo"
"$CLIENT_BIN" 127.0.0.1:8101 "$TRACKER_INFO" < "$WORKDIR/alice_fifo" > "$WORKDIR/alice.out" 2>&1 &
ALICE_PID=$!
exec 3>"$WORKDIR/alice_fifo"
cat >&3 <<CMDS
create user alice pw1
login alice pw1
create group g1
upload file g1 $WORKDIR/A.bin
upload file g1 $WORKDIR/B.bin
CMDS
sleep 1

# --- bob and carol register + request to join ---
printf 'create user bob pw2\nlogin bob pw2\njoin group g1\nlogout\n'   | "$CLIENT_BIN" 127.0.0.1:8199 "$TRACKER_INFO" > /dev/null 2>&1
printf 'create user carol pw3\nlogin carol pw3\njoin group g1\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8198 "$TRACKER_INFO" > /dev/null 2>&1
printf 'accept request g1 bob\naccept request g1 carol\n' >&3
sleep 0.5

# --- bob: downloads A and B from alice, then stays online ---
mkfifo "$WORKDIR/bob_fifo"
"$CLIENT_BIN" 127.0.0.1:8102 "$TRACKER_INFO" < "$WORKDIR/bob_fifo" > "$WORKDIR/bob.out" 2>&1 &
BOB_PID=$!
exec 4>"$WORKDIR/bob_fifo"
printf 'login bob pw2\ndownload file g1 A.bin %s/bob_A.bin\ndownload file g1 B.bin %s/bob_B.bin\n' "$WORKDIR" "$WORKDIR" >&4

echo "=== Step 2: bob downloads from alice ==="
wait_for "$WORKDIR/bob.out" "\[C\] \[g1\] A.bin" && ok "bob completed A.bin" || bad "bob did not complete A.bin"
wait_for "$WORKDIR/bob.out" "\[C\] \[g1\] B.bin" && ok "bob completed B.bin" || bad "bob did not complete B.bin"
sleep 0.5
grep -q "Warning: download succeeded" "$WORKDIR/bob.out" && bad "bob's seeder announcement was rejected" || ok "bob's seeder announcement accepted (no warning)"

echo "=== Step 3: alice stops sharing B.bin ==="
printf 'stop share g1 B.bin\n' >&3
sleep 0.7

echo "=== Step 4/5: carol downloads A (2 seeders) and B (only bob) ==="
printf 'login carol pw3\ndownload file g1 A.bin %s/carol_A.bin\ndownload file g1 B.bin %s/carol_B.bin\nlogout\n' "$WORKDIR" "$WORKDIR" \
  | "$CLIENT_BIN" 127.0.0.1:8103 "$TRACKER_INFO" > "$WORKDIR/carol.out" 2>&1

grep -q "from 2 peers" "$WORKDIR/carol.out" && ok "A.bin used the multi-peer path (2 seeders: alice + bob)" || bad "A.bin was not downloaded from 2 peers"
for f in A B; do
    grep -q "\[C\] \[g1\] $f.bin" "$WORKDIR/carol.out" && ok "carol completed $f.bin" || bad "carol did not complete $f.bin"
    if [ "$(sha1sum < "$WORKDIR/$f.bin")" = "$(sha1sum < "$WORKDIR/carol_$f.bin" 2>/dev/null)" ]; then
        ok "carol's $f.bin SHA1 matches original"
    else
        bad "carol's $f.bin differs from original"
    fi
done

echo "=== Tracker health ==="
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR/tracker.log" && bad "tracker crashed" || ok "tracker survived"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
