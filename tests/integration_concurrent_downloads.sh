#!/bin/bash
# Integration test: concurrent downloads + `show downloads`.
#
# Scenario (1 tracker, 2 clients):
#   1. alice uploads two large files (A.bin, B.bin) and one small one (C.bin).
#   2. bob issues `download file` for A and B back to back, then
#      `show downloads`, without waiting for either to finish.
#   3. While they run, bob also tries (a) the same file again and (b) a
#      different file into a destination that is already in use. Both must be
#      refused.
#   4. Both downloads must finish, `show downloads` must report them as [C],
#      and the bytes must match the originals exactly.
#   5. A fresh client with no downloads must print "No downloads.".
#
# Run from anywhere:  bash tests/integration_concurrent_downloads.sh
# Optional: SIZE_MB=64 bash tests/integration_concurrent_downloads.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
TRACKER_INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
SIZE_MB=${SIZE_MB:-96}
PASS=0; FAIL=0
TRACKER_PID=""; ALICE_PID=""; BOB_PID=""

cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in $ALICE_PID $BOB_PID $TRACKER_PID; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# wait_for FILE PATTERN [TENTHS_OF_A_SECOND]
wait_for() {
    for _ in $(seq 1 ${3:-600}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}

echo "=== Creating test files (2 x ${SIZE_MB} MB + 1 small) ==="
head -c $((SIZE_MB*1024*1024 + 4321)) /dev/urandom > "$WORKDIR/A.bin"
head -c $((SIZE_MB*1024*1024 + 999))  /dev/urandom > "$WORKDIR/B.bin"
head -c 3000 /dev/urandom > "$WORKDIR/C.bin"

"$TRACKER_BIN" "$TRACKER_INFO" 1 > "$WORKDIR/tracker.log" 2>&1 < /dev/null &
TRACKER_PID=$!
sleep 0.5

# --- alice: uploader, kept alive through a FIFO ---
mkfifo "$WORKDIR/alice_fifo"
"$CLIENT_BIN" 127.0.0.1:8401 "$TRACKER_INFO" < "$WORKDIR/alice_fifo" > "$WORKDIR/alice.out" 2>&1 &
ALICE_PID=$!
exec 3>"$WORKDIR/alice_fifo"
cat >&3 <<CMDS
create user alice pw1
login alice pw1
create group g1
upload file g1 $WORKDIR/A.bin
upload file g1 $WORKDIR/B.bin
upload file g1 $WORKDIR/C.bin
CMDS
# Hashing large files takes a moment: wait until all three uploads answered OK.
for _ in $(seq 1 300); do
    [ "$(grep -c '^OK' "$WORKDIR/alice.out" 2>/dev/null)" -ge 6 ] && break
    sleep 0.1
done

# --- bob and carol: register, join, get accepted (short-lived clients) ---
printf 'create user bob pw2\nlogin bob pw2\njoin group g1\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8499 "$TRACKER_INFO" > /dev/null 2>&1
printf 'create user carol pw3\nlogin carol pw3\njoin group g1\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8498 "$TRACKER_INFO" > /dev/null 2>&1
printf 'accept request g1 bob\naccept request g1 carol\n' >&3
sleep 0.5

# --- bob: the client under test ---
mkfifo "$WORKDIR/bob_fifo"
"$CLIENT_BIN" 127.0.0.1:8402 "$TRACKER_INFO" < "$WORKDIR/bob_fifo" > "$WORKDIR/bob.out" 2>&1 &
BOB_PID=$!
exec 4>"$WORKDIR/bob_fifo"

echo "=== Step 1: two downloads back to back, then show downloads ==="
printf 'login bob pw2\n' >&4
sleep 0.5
# All of these are sent at once. If downloads still blocked the prompt, the
# later commands could not be processed until A had finished.
printf 'download file g1 A.bin %s/bob_A.bin\n' "$WORKDIR" >&4
printf 'download file g1 B.bin %s/bob_B.bin\n' "$WORKDIR" >&4
printf 'show downloads\n' >&4
printf 'download file g1 A.bin %s/bob_A_second.bin\n' "$WORKDIR" >&4   # same file, already running
printf 'download file g1 C.bin %s/bob_B.bin\n' "$WORKDIR" >&4           # destination already in use

wait_for "$WORKDIR/bob.out" "destination .* already in use" 100 && ok "same-destination request refused" || bad "same-destination request was not refused"
grep -q "is already being downloaded" "$WORKDIR/bob.out" && ok "duplicate download of the same file refused" || bad "duplicate download not refused"

# The prompt must have accepted the second download before the first finished.
LINE_START_B=$(grep -n "Download started: \[g1\] B.bin" "$WORKDIR/bob.out" | head -1 | cut -d: -f1)
LINE_DONE_A=$(grep -n "^\[C\] \[g1\] A.bin" "$WORKDIR/bob.out" | head -1 | cut -d: -f1)
if [ -n "$LINE_START_B" ] && { [ -z "$LINE_DONE_A" ] || [ "$LINE_START_B" -lt "$LINE_DONE_A" ]; }; then
    ok "second download was accepted while the first was still running (prompt not blocked)"
else
    bad "second download only started after the first finished (start line=$LINE_START_B, A done line=$LINE_DONE_A)"
fi

# The first `show downloads` ran right after both started: it should list them.
if grep -q "^\[D\] \[g1\] A.bin" "$WORKDIR/bob.out" && grep -q "^\[D\] \[g1\] B.bin" "$WORKDIR/bob.out"; then
    ok "show downloads listed both files as in progress ([D])"
else
    echo "  NOTE: no [D] line captured (downloads may have finished before the first show downloads ran)"
fi

echo "=== Step 2: both finish ==="
wait_for "$WORKDIR/bob.out" "^\[C\] \[g1\] A.bin" && ok "A.bin reported complete" || bad "A.bin never completed"
wait_for "$WORKDIR/bob.out" "^\[C\] \[g1\] B.bin" && ok "B.bin reported complete" || bad "B.bin never completed"
sleep 0.5
: > /dev/null
printf 'show downloads\n' >&4
sleep 0.7
# Everything after the LAST "Download started" is the final listing.
FINAL=$(tail -n 5 "$WORKDIR/bob.out")
echo "$FINAL" | grep -q "^\[C\] \[g1\] A.bin" && echo "$FINAL" | grep -q "^\[C\] \[g1\] B.bin" \
    && ok "final show downloads lists both as [C]" || bad "final show downloads did not list both as [C]"

for f in A B; do
    if [ "$(sha1sum < "$WORKDIR/$f.bin")" = "$(sha1sum < "$WORKDIR/bob_$f.bin" 2>/dev/null)" ]; then
        ok "bob's $f.bin SHA1 matches the original"
    else
        bad "bob's $f.bin differs from the original"
    fi
done
grep -q "Warning: download succeeded" "$WORKDIR/bob.out" && bad "seeder announcement was rejected" || ok "both downloads announced bob as a seeder"

echo "=== Step 2b: carol downloads BOTH big files at once, each from 2 seeders (alice + bob) ==="
printf 'login carol pw3\ndownload file g1 A.bin %s/carol_A.bin\ndownload file g1 B.bin %s/carol_B.bin\nshow downloads\nlogout\n' \
    "$WORKDIR" "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:8404 "$TRACKER_INFO" > "$WORKDIR/carol.out" 2>&1
[ "$(grep -c 'from 2 peers' "$WORKDIR/carol.out")" -eq 2 ] && ok "both concurrent downloads used the multi-peer path" || bad "expected 2 multi-peer downloads in carol's output"
for f in A B; do
    grep -q "^\[C\] \[g1\] $f.bin" "$WORKDIR/carol.out" && ok "carol completed $f.bin" || bad "carol did not complete $f.bin"
    if [ "$(sha1sum < "$WORKDIR/$f.bin")" = "$(sha1sum < "$WORKDIR/carol_$f.bin" 2>/dev/null)" ]; then
        ok "carol's $f.bin SHA1 matches the original"
    else
        bad "carol's $f.bin differs from the original"
    fi
done

echo "=== Step 3: logout waits for a running download; quit then exits cleanly ==="
printf 'download file g1 C.bin %s/bob_C.bin\nlogout\nquit\n' "$WORKDIR" >&4
for _ in $(seq 1 100); do kill -0 "$BOB_PID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$BOB_PID" 2>/dev/null && bad "bob did not exit after logout" || ok "bob exited after logout"
grep -q "^\[C\] \[g1\] C.bin" "$WORKDIR/bob.out" && ok "C.bin finished before logout completed" || bad "C.bin lost when logging out"
[ "$(sha1sum < "$WORKDIR/C.bin")" = "$(sha1sum < "$WORKDIR/bob_C.bin" 2>/dev/null)" ] && ok "C.bin SHA1 matches" || bad "C.bin differs"

echo "=== Step 4: empty table ==="
OUT=$(printf 'show downloads\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8403 "$TRACKER_INFO" 2>&1)
echo "$OUT" | grep -q "No downloads." && ok "empty table prints 'No downloads.'" || bad "empty table output wrong"
echo "$OUT" | grep -q "Unknown command" && bad "show downloads still reported as unknown" || ok "show downloads is a recognised command"

echo "=== Health ==="
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR/tracker.log" "$WORKDIR/bob.out" "$WORKDIR/alice.out" "$WORKDIR/carol.out" \
    && bad "a process crashed" || ok "no crashes"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
