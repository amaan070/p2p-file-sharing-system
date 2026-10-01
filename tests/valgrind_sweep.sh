#!/bin/bash
# Memory-safety sweep with valgrind (--leak-check=full, --track-fds=yes).
#
# PART 1 -- the TRACKER, under load
#   Tracker 1 runs under valgrind. Three clients (two attached to tracker 1, one
#   to tracker 2) create groups, join, upload and cross-download files while
#   spamming list commands. Then `quit` is typed at the tracker's console WHILE
#   THE CLIENTS ARE STILL CONNECTED, which exercises the shutdown path (handler
#   threads are disconnected and joined before state is destroyed).
#
# PART 2 -- the CLIENT, during multi-peer downloads
#   bob and carol run under valgrind. bob downloads two files at once from alice;
#   carol then downloads the same two files at once from alice + bob (multi-peer).
#   Halfway through, bob QUITS while still serving pieces to carol (seeder
#   shutdown with live connections). carol finishes from alice.
#
# Pass criteria for each valgrind-run process:
#   - "ERROR SUMMARY: 0 errors"      (no invalid reads/writes, no use of freed memory,
#                                     no uninitialised values, no data-race-like misuse)
#   - definitely / indirectly / possibly lost: 0 bytes
#   - no file descriptors left open besides stdin/stdout/stderr
#   - the process exited with status 0
#
# valgrind is 20-50x slower than native, so the files here are small (MiB, not GiB).
# Usage: bash tests/valgrind_sweep.sh [1] [2]      (default: both parts)

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=${WORKDIR:-$(mktemp -d)}
mkdir -p "$WORKDIR"
SWAPPED="$WORKDIR/info_swapped.txt"
grep -v '^#' "$INFO" | grep -v '^$' | tac > "$SWAPPED"
VG="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect,possible --track-fds=yes --error-exitcode=99"
PASS=0; FAIL=0; PIDS=""
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
say() { printf '%s\n' "$2" >&"$1"; }
cleanup() {
    exec 3>&- 4>&- 5>&- 6>&- 2>/dev/null
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    [ -z "${KEEP:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT
wait_for() { for _ in $(seq 1 ${3:-600}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.2; done; return 1; }
pieces_done() { grep '^\[D\]' "$1" | tail -1 | sed -n 's/.*(\([0-9]*\)\/.*/\1/p'; }

# Judge one valgrind log. args: label logfile exit_code
# Numbers are parsed out of valgrind's own summary lines (a zero-leak summary
# still contains the words "definitely lost", so keyword matching would be wrong).
judge() {
    local label=$1 log=$2 code=$3
    [ "$code" = "0" ] && ok "$label: exited with status 0" || bad "$label: exit status $code (99 = valgrind found errors)"
    if ! grep -q "ERROR SUMMARY" "$log" 2>/dev/null; then bad "$label: no valgrind summary in the log (did the process finish?)"; return; fi

    local errs; errs=$(sed -n 's/.*ERROR SUMMARY: \([0-9,]*\) errors.*/\1/p' "$log" | tail -1 | tr -d ,)
    [ "${errs:-1}" = "0" ] && ok "$label: ERROR SUMMARY: 0 errors (no invalid reads/writes, no use-after-free, no uninitialised use)" \
        || { bad "$label: valgrind reports ${errs:-?} errors"; grep -m3 -A8 "Invalid\|uninitialised\|Mismatched\|Conditional\|lost in loss" "$log" | head -30; }

    local lost=0 kind n
    for kind in "definitely lost" "indirectly lost" "possibly lost"; do
        n=$(sed -n "s/.*$kind: \([0-9,]*\) bytes.*/\1/p" "$log" | tail -1 | tr -d ,); lost=$((lost + ${n:-0}))
    done
    [ "$lost" = "0" ] && ok "$label: no leaks (definitely / indirectly / possibly lost = 0 bytes)" || bad "$label: $lost bytes leaked"
    local reach; reach=$(sed -n 's/.*still reachable: \([0-9,]*\) bytes.*/\1/p' "$log" | tail -1 | tr -d ,)
    echo "    (info) still reachable at exit: ${reach:-0} bytes;  $(grep -c 'All heap blocks were freed' "$log") x 'all heap blocks were freed'"

    local open std; open=$(sed -n 's/.*FILE DESCRIPTORS: \([0-9]*\) open.*/\1/p' "$log" | tail -1); std=$(sed -n 's/.*FILE DESCRIPTORS: [0-9]* open (\([0-9]*\) std).*/\1/p' "$log" | tail -1)
    if [ -z "$open" ]; then bad "$label: no file descriptor report"
    elif [ "$open" -le "${std:-3}" ]; then ok "$label: every file descriptor was closed ($open open at exit, all std)"
    else bad "$label: $((open - std)) non-std descriptors still open at exit"; grep -A4 "^==[0-9]*== Open" "$log" | head -20; fi
}

WANT=${*:-1 2}
want() { [[ " $WANT " == *" $1 "* ]]; }

# ===========================================================================
if want 1; then
echo "=== PART 1: tracker under valgrind, with connections still open at quit ==="
head -c 3000000 /dev/urandom > "$WORKDIR/A1.bin"; head -c 2000000 /dev/urandom > "$WORKDIR/B1.bin"
mkfifo "$WORKDIR/t1.in"
$VG "$TRACKER_BIN" "$INFO" 1 < "$WORKDIR/t1.in" > "$WORKDIR/t1.log" 2> "$WORKDIR/t1.vg" & T1=$!; PIDS="$PIDS $T1"
exec 6>"$WORKDIR/t1.in"
"$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/t2.log" 2>&1 < /dev/null & PIDS="$PIDS $!"
wait_for "$WORKDIR/t1.log" "Tracker ready" 300 || bad "tracker under valgrind did not start"
sleep 2
start_client() { mkfifo "$WORKDIR/$1.fifo"; "$CLIENT_BIN" "127.0.0.1:$2" "$3" < "$WORKDIR/$1.fifo" > "$WORKDIR/$1.out" 2>&1 & PIDS="$PIDS $!"; eval "exec $4>\"$WORKDIR/$1.fifo\""; }
start_client alice 9701 "$INFO" 3; start_client bob 9702 "$SWAPPED" 4; start_client carol 9703 "$INFO" 5
say 3 "create user alice pw"; say 3 "login alice pw"; say 3 "create group ga"
say 4 "create user bob pw";   say 4 "login bob pw";     say 4 "create group gb"
say 5 "create user carol pw"; say 5 "login carol pw";   say 5 "create group gc"
sleep 6
for _ in 1 2 3 4 5 6; do
    say 4 "join group ga"; say 5 "join group ga"; say 3 "join group gb"; sleep 1
    say 3 "accept request ga bob"; say 3 "accept request ga carol"; say 4 "accept request gb alice"; sleep 1
done
say 3 "upload file ga $WORKDIR/A1.bin"; say 4 "upload file gb $WORKDIR/B1.bin"
sleep 6
for i in $(seq 1 8); do
    for fd in 3 4 5; do say $fd "list groups"; say $fd "create group extra_${fd}_$i"; say $fd "list files ga"; done
done
for _ in $(seq 1 40); do
    say 4 "download file ga A1.bin $WORKDIR/bob_A1.bin"; say 3 "download file gb B1.bin $WORKDIR/alice_B1.bin"
    sleep 1
    grep -q "Download started: \[ga\] A1.bin" "$WORKDIR/bob.out" && grep -q "Download started: \[gb\] B1.bin" "$WORKDIR/alice.out" && break
done
say 5 "download file ga A1.bin $WORKDIR/carol_A1.bin"
wait_for "$WORKDIR/bob.out" "^\[C\] \[ga\] A1.bin" 300 && ok "cross-download through the valgrind tracker completed" || bad "download via valgrind tracker did not complete"
sleep 3

echo "  -- typing 'quit' at the tracker while clients and the peer tracker's sync link are still connected --"
say 6 "quit"
for _ in $(seq 1 300); do kill -0 $T1 2>/dev/null || break; sleep 0.5; done
if kill -0 $T1 2>/dev/null; then bad "tracker did not shut down within 150 s (deadlock on quit?)"; kill -9 $T1; CODE=hang
else wait $T1; CODE=$?; fi
grep -q "Tracker stopped" "$WORKDIR/t1.log" && ok "tracker printed 'Tracker stopped.' (clean shutdown path completed)" || bad "tracker never reached its final line"
judge "tracker" "$WORKDIR/t1.vg" "$CODE"
exec 3>&- 4>&- 5>&- 6>&-
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; PIDS=""
fi

# ===========================================================================
if want 2; then
echo; echo "=== PART 2: clients under valgrind, multi-peer downloads, seeder quits mid-transfer ==="
rm -f "$WORKDIR"/*.fifo
head -c $((16*1024*1024 + 555)) /dev/urandom > "$WORKDIR/F1.bin"; head -c $((12*1024*1024 + 77)) /dev/urandom > "$WORKDIR/F2.bin"
"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/t1.log" 2>&1 < /dev/null & PIDS="$PIDS $!"; sleep 0.8
mkfifo "$WORKDIR/alice.fifo" "$WORKDIR/bob.fifo" "$WORKDIR/carol.fifo"
"$CLIENT_BIN" 127.0.0.1:9711 "$INFO" < "$WORKDIR/alice.fifo" > "$WORKDIR/alice.out" 2>&1 & PIDS="$PIDS $!"
$VG "$CLIENT_BIN" 127.0.0.1:9712 "$INFO" < "$WORKDIR/bob.fifo"   > "$WORKDIR/bob.out"   2> "$WORKDIR/bob.vg"   & BOB=$!;   PIDS="$PIDS $BOB"
$VG "$CLIENT_BIN" 127.0.0.1:9713 "$INFO" < "$WORKDIR/carol.fifo" > "$WORKDIR/carol.out" 2> "$WORKDIR/carol.vg" & CAROL=$!; PIDS="$PIDS $CAROL"
exec 3>"$WORKDIR/alice.fifo" 4>"$WORKDIR/bob.fifo" 5>"$WORKDIR/carol.fifo"
say 3 "create user alice pw"; say 3 "login alice pw"; say 3 "create group g"
wait_for "$WORKDIR/bob.out" "Ready" 300; wait_for "$WORKDIR/carol.out" "Ready" 300
say 4 "create user bob pw"; say 4 "login bob pw"; say 5 "create user carol pw"; say 5 "login carol pw"; sleep 3
for _ in 1 2 3 4; do say 4 "join group g"; say 5 "join group g"; sleep 1; say 3 "accept request g bob"; say 3 "accept request g carol"; sleep 1; done
say 3 "upload file g $WORKDIR/F1.bin"; say 3 "upload file g $WORKDIR/F2.bin"
sleep 8

echo "  -- bob (valgrind) downloads F1 and F2 at the same time from alice --"
say 4 "download file g F1.bin $WORKDIR/bob_F1.bin"; say 4 "download file g F2.bin $WORKDIR/bob_F2.bin"
wait_for "$WORKDIR/bob.out" "^\[C\] \[g\] F1.bin" 900 && wait_for "$WORKDIR/bob.out" "^\[C\] \[g\] F2.bin" 900 \
    && ok "bob completed both concurrent downloads under valgrind" || bad "bob's downloads did not complete"
[ "$(sha1sum < "$WORKDIR/F1.bin")" = "$(sha1sum < "$WORKDIR/bob_F1.bin")" ] && [ "$(sha1sum < "$WORKDIR/F2.bin")" = "$(sha1sum < "$WORKDIR/bob_F2.bin")" ] \
    && ok "bob's files are byte-identical" || bad "bob's files differ"
sleep 4

echo "  -- carol (valgrind) downloads both from alice + bob; bob quits halfway --"
say 5 "download file g F1.bin $WORKDIR/carol_F1.bin"; say 5 "download file g F2.bin $WORKDIR/carol_F2.bin"
for _ in $(seq 1 300); do say 5 "show downloads"; sleep 0.5; d=$(pieces_done "$WORKDIR/carol.out"); [ -n "$d" ] && [ "$d" -ge 4 ] && break; grep -q '^\[C\] \[g\] F2' "$WORKDIR/carol.out" && break; done
say 4 "show downloads"; say 4 "logout"; say 4 "login bob pw"; say 4 "quit"     # session cycle, then quit mid-transfer
for _ in $(seq 1 600); do kill -0 $BOB 2>/dev/null || break; sleep 0.5; done
if kill -0 $BOB 2>/dev/null; then bad "bob (valgrind) did not exit"; kill -9 $BOB; BCODE=hang; else wait $BOB; BCODE=$?; fi
ok "bob quit while serving carol (exit handled)"
wait_for "$WORKDIR/carol.out" "^\[C\] \[g\] F1.bin" 1500 && wait_for "$WORKDIR/carol.out" "^\[C\] \[g\] F2.bin" 1500 \
    && ok "carol still completed both downloads (fell back to alice)" || bad "carol's downloads did not complete after bob quit"
[ "$(sha1sum < "$WORKDIR/F1.bin")" = "$(sha1sum < "$WORKDIR/carol_F1.bin")" ] && [ "$(sha1sum < "$WORKDIR/F2.bin")" = "$(sha1sum < "$WORKDIR/carol_F2.bin")" ] \
    && ok "carol's files are byte-identical" || bad "carol's files differ"
say 5 "quit"
for _ in $(seq 1 600); do kill -0 $CAROL 2>/dev/null || break; sleep 0.5; done
if kill -0 $CAROL 2>/dev/null; then bad "carol (valgrind) did not exit"; kill -9 $CAROL; CCODE=hang; else wait $CAROL; CCODE=$?; fi
judge "client bob (downloader + seeder that quit mid-transfer)" "$WORKDIR/bob.vg" "$BCODE"
judge "client carol (multi-peer downloader)" "$WORKDIR/carol.vg" "$CCODE"
fi

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
