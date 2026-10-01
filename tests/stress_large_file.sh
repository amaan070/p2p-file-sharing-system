#!/bin/bash
# Large-file stress test.
#
# Question being answered: does a ~1 GB file stay streamed piece-by-piece
# (small, constant memory) through upload-hashing, single-peer download and
# multi-peer download, and does it arrive byte-for-byte intact?
#
#   1. alice uploads a big file        -> peak RSS of an uploader/seeder
#   2. bob downloads it from alice     -> peak RSS of a single-peer download
#   3. carol downloads it from alice AND bob (2 seeders, multi-peer path)
#
# Peak memory is measured with GNU time (-v: "Maximum resident set size").
# Each client is asked to `quit` at the end so GNU time can report on it.
#
# Usage: bash tests/stress_large_file.sh [SIZE_BYTES]      (default: 1 GiB)

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
TRACKER_INFO="$PROJECT_DIR/tracker_info.txt"
SIZE=${1:-1073741824}
WORKDIR=${WORKDIR:-$(mktemp -d)}
mkdir -p "$WORKDIR"
GNUTIME=/usr/bin/time
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
now() { date +%s.%N; }
elapsed() { printf "%.1f" "$(echo "$2 - $1" | bc)"; }
peak_mb() { awk '/Maximum resident/ {printf "%.1f", $6/1024}' "$1"; }
wait_for() { for _ in $(seq 1 ${3:-3000}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }

TRACKER_PID=""; PIDS=""
cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null
    for p in $PIDS $TRACKER_PID; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    [ -z "${KEEP:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

[ -x "$GNUTIME" ] || { echo "GNU time not found at $GNUTIME"; exit 2; }

echo "=== Creating $((SIZE/1048576)) MiB test file ($SIZE bytes) ==="
head -c "$SIZE" /dev/urandom > "$WORKDIR/big.bin"
PIECES=$(( (SIZE + 524287) / 524288 ))
echo "    $PIECES pieces of 512 KiB"
ORIG_SHA=$(sha1sum < "$WORKDIR/big.bin")

"$TRACKER_BIN" "$TRACKER_INFO" 1 > "$WORKDIR/tracker.log" 2>&1 < /dev/null &
TRACKER_PID=$!
sleep 0.5

start_client() {   # name port fifo-fd-number
    mkfifo "$WORKDIR/$1_fifo"
    $GNUTIME -v -o "$WORKDIR/$1.time" "$CLIENT_BIN" "127.0.0.1:$2" "$TRACKER_INFO" \
        < "$WORKDIR/$1_fifo" > "$WORKDIR/$1.out" 2>&1 &
    PIDS="$PIDS $!"
}

echo "=== Stage 1: alice uploads (streams the file through SHA1) ==="
start_client alice 8701
exec 3>"$WORKDIR/alice_fifo"
printf 'create user alice pw1\nlogin alice pw1\ncreate group g1\n' >&3
sleep 0.5
T0=$(now)
printf 'upload file g1 %s/big.bin\n' "$WORKDIR" >&3
for _ in $(seq 1 6000); do [ "$(grep -c '^OK' "$WORKDIR/alice.out")" -ge 4 ] && break; sleep 0.1; done
T1=$(now)
[ "$(grep -c '^OK' "$WORKDIR/alice.out")" -ge 4 ] && ok "upload accepted by tracker" || bad "upload did not complete"
UPLOAD_S=$(elapsed "$T0" "$T1")

printf 'create user bob pw2\nlogin bob pw2\njoin group g1\nlogout\n'   | "$CLIENT_BIN" 127.0.0.1:8799 "$TRACKER_INFO" >/dev/null 2>&1
printf 'create user carol pw3\nlogin carol pw3\njoin group g1\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8798 "$TRACKER_INFO" >/dev/null 2>&1
printf 'accept request g1 bob\naccept request g1 carol\n' >&3
sleep 0.5

echo "=== Stage 2: bob downloads from alice only (single-peer path) ==="
start_client bob 8702
exec 4>"$WORKDIR/bob_fifo"
printf 'login bob pw2\n' >&4; sleep 0.4
T0=$(now)
printf 'download file g1 big.bin %s/bob_big.bin\n' "$WORKDIR" >&4
wait_for "$WORKDIR/bob.out" "^\[C\] \[g1\] big.bin" 6000 && ok "bob: download reported complete" || bad "bob: download did not complete"
T1=$(now); BOB_S=$(elapsed "$T0" "$T1")
sleep 1   # let bob's seeder announcement land
[ "$ORIG_SHA" = "$(sha1sum < "$WORKDIR/bob_big.bin" 2>/dev/null)" ] && ok "bob: SHA1 of downloaded file matches original" || bad "bob: file differs from original"

echo "=== Stage 3: carol downloads from alice + bob (multi-peer path) ==="
start_client carol 8703
exec 5>"$WORKDIR/carol_fifo"
printf 'login carol pw3\n' >&5; sleep 0.4
T0=$(now)
printf 'download file g1 big.bin %s/carol_big.bin\n' "$WORKDIR" >&5
wait_for "$WORKDIR/carol.out" "^\[C\] \[g1\] big.bin" 6000 && ok "carol: download reported complete" || bad "carol: download did not complete"
T1=$(now); CAROL_S=$(elapsed "$T0" "$T1")
grep -q "from 2 peers" "$WORKDIR/carol.out" && ok "carol: used the multi-peer path (2 seeders)" || bad "carol: multi-peer path NOT used"
[ "$ORIG_SHA" = "$(sha1sum < "$WORKDIR/carol_big.bin" 2>/dev/null)" ] && ok "carol: SHA1 of downloaded file matches original" || bad "carol: file differs from original"

# Ask everyone to quit so GNU time can print its report.
printf 'quit\n' >&5; printf 'quit\n' >&4; printf 'quit\n' >&3
sleep 3

echo
echo "=== Results for a $((SIZE/1048576)) MiB file ($PIECES pieces) ==="
printf "  %-34s %8s   %s\n" "stage" "seconds" "peak RSS (client)"
printf "  %-34s %8s   %s MiB\n" "alice: upload (hash whole file)"   "$UPLOAD_S" "$(peak_mb "$WORKDIR/alice.time")"
printf "  %-34s %8s   %s MiB\n" "bob:   download, 1 seeder"          "$BOB_S"    "$(peak_mb "$WORKDIR/bob.time")"
printf "  %-34s %8s   %s MiB\n" "carol: download, 2 seeders"         "$CAROL_S"  "$(peak_mb "$WORKDIR/carol.time")"
echo "  (alice's peak also includes serving pieces to bob and carol)"
echo "  file size for comparison: $((SIZE/1048576)) MiB"

# Streaming means memory must be tiny compared to the file. Generous bound: 64 MiB.
for c in alice bob carol; do
    P=$(peak_mb "$WORKDIR/$c.time")
    [ -n "$P" ] && awk -v p="$P" 'BEGIN{exit !(p < 64)}' && ok "$c peak RSS ${P} MiB is far below the file size (streamed)" || bad "$c peak RSS ${P:-unknown} MiB is too high"
done
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.out "$WORKDIR/tracker.log" && bad "a process crashed" || ok "no crashes"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
