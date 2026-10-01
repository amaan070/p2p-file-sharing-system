#!/bin/bash
# Failure injection.
#
#   A  a seeder is SIGKILLed in the middle of a download (another seeder remains)
#   B  the ONLY seeder is SIGKILLed mid-download -> clean failure, client stays usable
#   C  a seeder serves corrupted data (its file was tampered with) -> re-fetched elsewhere
#   D  a TRACKER is SIGKILLed while a transfer is running -> transfer + seeder announcement survive
#   E  NETWORK PARTITION between the two trackers (split brain): both keep serving
#      clients and diverge, then the link heals and they must converge
#   F  a tracker is FROZEN (SIGSTOP: connections stay open but nothing answers), a
#      "blackhole" partition -> clients fail over; after SIGCONT everything reconverges
#
# Scenarios A-D use one big file and inject the fault when the download is
# demonstrably in progress (polled through `show downloads`), so the fault really
# lands mid-transfer, not before or after it.
#
# Usage: bash tests/failure_injection.sh [A B C ...]     (default: all scenarios)
#        SIZE_MB=64 bash tests/failure_injection.sh      (smaller files, faster)

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
PROXY_BIN="$PROJECT_DIR/tests/tcp_proxy"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=${WORKDIR:-$(mktemp -d)}
mkdir -p "$WORKDIR"
SWAPPED="$WORKDIR/info_swapped.txt"
grep -v '^#' "$INFO" | grep -v '^$' | tac > "$SWAPPED"
SIZE_MB=${SIZE_MB:-128}
PASS=0; FAIL=0
PIDS=""

ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
say() { printf '%s\n' "$2" >&"$1"; }                          # say FD "command"

[ -x "$PROXY_BIN" ] || g++ -std=c++17 -pthread "$PROJECT_DIR/tests/tcp_proxy.cpp" -o "$PROXY_BIN" || exit 2

teardown() {
    exec 3>&- 4>&- 5>&- 6>&- 2>/dev/null
    for p in $PIDS; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    PIDS=""
    rm -f "$WORKDIR"/*.fifo
    sleep 1
}
trap 'teardown; [ -z "${KEEP:-}" ] && rm -rf "$WORKDIR"' EXIT

wait_for() {  # file pattern [tenths]
    for _ in $(seq 1 ${3:-300}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}
start_tracker() {  # no infofile logname   -> sets LAST_PID
    "$TRACKER_BIN" "$2" "$1" > "$WORKDIR/$3.log" 2>&1 < /dev/null &
    LAST_PID=$!; PIDS="$PIDS $LAST_PID"
}
start_client() {   # name port infofile fd   -> sets LAST_PID
    mkfifo "$WORKDIR/$1.fifo"
    "$CLIENT_BIN" "127.0.0.1:$2" "$3" < "$WORKDIR/$1.fifo" > "$WORKDIR/$1.out" 2>&1 &
    LAST_PID=$!; PIDS="$PIDS $LAST_PID"
    eval "exec $4>\"$WORKDIR/$1.fifo\""
}
# Number of pieces done, from the LAST "[D]" line a client has printed.
pieces_done() { grep '^\[D\]' "$1" | tail -1 | sed -n 's/.*(\([0-9]*\)\/.*/\1/p'; }
# Poll `show downloads` until at least N pieces are done. Returns 1 if the download finished first.
wait_progress() {  # fd outfile N
    for _ in $(seq 1 400); do
        say "$1" "show downloads"; sleep 0.15
        local d; d=$(pieces_done "$2")
        [ -n "$d" ] && [ "$d" -ge "$3" ] && return 0
        grep -q '^\[C\]' "$2" && return 1
    done
    return 1
}
sha_of() { sha1sum < "$1" 2>/dev/null; }
# The most recent "OK: ..." line the client printed, split on '|', sorted.
last_ok_list() { grep '^OK: ' "$1" | tail -1 | sed 's/^OK: //' | tr '|' '\n' | sort; }
list_of() {  # fd outfile command -> prints sorted items of the answer
    local before; before=$(grep -c '^OK: ' "$2")
    say "$1" "$3"
    for _ in $(seq 1 60); do [ "$(grep -c '^OK: ' "$2")" -gt "$before" ] && break; sleep 0.1; done
    last_ok_list "$2"
}

# alice owns group g; bob and carol are accepted members. All three logged in.
# args: infoA (alice, bob) infoC (carol)
bootstrap() {
    start_client alice 8951 "$1" 3; ALICE=$LAST_PID
    start_client bob   8952 "$1" 4; BOB=$LAST_PID
    start_client carol 8953 "${2:-$1}" 5; CAROL=$LAST_PID
    say 3 "create user alice pw"; say 3 "login alice pw"; say 3 "create group g"
    say 4 "create user bob pw";   say 4 "login bob pw"
    say 5 "create user carol pw"; say 5 "login carol pw"; sleep 0.8
    for _ in 1 2 3 4 5; do say 4 "join group g"; say 5 "join group g"; sleep 0.3
                           say 3 "accept request g bob"; say 3 "accept request g carol"; sleep 0.3; done
}
make_big() { head -c $((SIZE_MB*1024*1024 + 4321)) /dev/urandom > "$WORKDIR/big.bin"; BIGSHA=$(sha_of "$WORKDIR/big.bin"); }
upload_big() { say 3 "upload file g $WORKDIR/big.bin"; wait_for "$WORKDIR/alice.out" "^OK" 50; sleep 4; }

WANT=${*:-A B C D E F}
want() { [[ " $WANT " == *" $1 "* ]]; }

echo "=== Creating ${SIZE_MB} MiB test file ==="
make_big

# ---------------------------------------------------------------------------
if want A; then
echo; echo "=== A. one of TWO seeders is SIGKILLed mid-download ==="
start_tracker 1 "$INFO" t1; sleep 0.6
bootstrap "$INFO"; upload_big
say 4 "download file g big.bin $WORKDIR/bob_big.bin"
wait_for "$WORKDIR/bob.out" "^\[C\] \[g\] big.bin" 900 || bad "setup: bob's download did not finish"
sleep 1.5   # bob announces himself as a seeder
say 5 "download file g big.bin $WORKDIR/carol_big.bin"
if wait_progress 5 "$WORKDIR/carol.out" 8; then
    kill -9 "$BOB"; ok "killed seeder bob mid-download (carol had $(pieces_done "$WORKDIR/carol.out") pieces)"
else
    bad "could not inject: carol's download finished before the fault"
fi
grep -q "from 2 peers" "$WORKDIR/carol.out" && ok "carol was downloading from 2 seeders" || bad "carol did not use 2 seeders"
wait_for "$WORKDIR/carol.out" "^\[C\] \[g\] big.bin" 900 && ok "carol's download still completed" || bad "carol's download did not complete"
[ "$BIGSHA" = "$(sha_of "$WORKDIR/carol_big.bin")" ] && ok "carol's file is byte-identical to the original" || bad "carol's file is WRONG"
teardown; rm -f "$WORKDIR"/*_big.bin
fi

# ---------------------------------------------------------------------------
if want B; then
echo; echo "=== B. the ONLY seeder is SIGKILLed mid-download ==="
start_tracker 1 "$INFO" t1; sleep 0.6
bootstrap "$INFO"; upload_big
say 5 "download file g big.bin $WORKDIR/carol_big.bin"
if wait_progress 5 "$WORKDIR/carol.out" 8; then
    kill -9 "$ALICE"; ok "killed the only seeder mid-download (carol had $(pieces_done "$WORKDIR/carol.out") pieces)"
else
    bad "could not inject: download finished before the fault"
fi
for _ in $(seq 1 300); do say 5 "show downloads"; sleep 0.2; grep -q '^\[F\] \[g\] big.bin' "$WORKDIR/carol.out" && break; done
grep -q '^\[F\] \[g\] big.bin' "$WORKDIR/carol.out" && ok "download reported as failed ([F]) instead of hanging" || bad "download never reported failure (hung?)"
B4=$(grep -c '^OK: ' "$WORKDIR/carol.out"); say 5 "list groups"; sleep 1
[ "$(grep -c '^OK: ' "$WORKDIR/carol.out")" -gt "$B4" ] && ok "carol's client is still responsive afterwards" || bad "carol's client stopped responding"
say 5 "logout"; say 5 "quit"
for _ in $(seq 1 100); do kill -0 "$CAROL" 2>/dev/null || break; sleep 0.2; done
kill -0 "$CAROL" 2>/dev/null && bad "carol's client did not exit after logout + quit" || ok "carol's client exits cleanly after logout + quit"
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.out "$WORKDIR"/*.log && bad "crash" || ok "no crashes"
teardown; rm -f "$WORKDIR"/*_big.bin
fi

# ---------------------------------------------------------------------------
if want C; then
echo; echo "=== C. a seeder serves CORRUPTED data ==="
start_tracker 1 "$INFO" t1; sleep 0.6
bootstrap "$INFO"; upload_big
say 4 "download file g big.bin $WORKDIR/bob_big.bin"
wait_for "$WORKDIR/bob.out" "^\[C\] \[g\] big.bin" 900 || bad "setup: bob's download did not finish"
sleep 1.5
head -c "$(stat -c %s "$WORKDIR/big.bin")" /dev/urandom > "$WORKDIR/bob_big.bin"   # tamper: same size, wrong bytes
ok "bob's copy (which he is still seeding) was replaced with garbage of the same size"
say 5 "download file g big.bin $WORKDIR/carol_big.bin"
wait_for "$WORKDIR/carol.out" "^\[C\] \[g\] big.bin" 900 && ok "carol's download completed despite the corrupt seeder" || bad "carol's download failed"
[ "$BIGSHA" = "$(sha_of "$WORKDIR/carol_big.bin")" ] && ok "carol's file is byte-identical to the original" || bad "carol's file is WRONG"
teardown; rm -f "$WORKDIR"/*_big.bin
fi

# ---------------------------------------------------------------------------
if want D; then
echo; echo "=== D. a TRACKER is SIGKILLed while a transfer is running ==="
start_tracker 1 "$INFO" t1; T1=$LAST_PID
start_tracker 2 "$INFO" t2; T2=$LAST_PID; sleep 1
bootstrap "$INFO" "$SWAPPED"        # alice, bob on tracker 1; carol on tracker 2
upload_big
say 5 "download file g big.bin $WORKDIR/carol_big.bin"
if wait_progress 5 "$WORKDIR/carol.out" 8; then
    kill -9 "$T2"; ok "killed tracker 2 (carol's tracker) mid-download"
else
    bad "could not inject: download finished before the fault"
fi
wait_for "$WORKDIR/carol.out" "^\[C\] \[g\] big.bin" 900 && ok "the transfer finished anyway (data flows peer-to-peer)" || bad "transfer did not finish"
[ "$BIGSHA" = "$(sha_of "$WORKDIR/carol_big.bin")" ] && ok "carol's file is byte-identical" || bad "carol's file is WRONG"
sleep 3
grep -q "Warning: download succeeded" "$WORKDIR/carol.out" && bad "carol could not announce herself as a seeder" || ok "carol announced herself as a seeder via failover to tracker 1"
say 4 "download file g big.bin $WORKDIR/bob_big.bin"
wait_for "$WORKDIR/bob.out" "^\[C\] \[g\] big.bin" 900 || true
grep -q "from 2 peers" "$WORKDIR/bob.out" && ok "bob was served by 2 seeders (alice + carol): carol's announcement reached tracker 1" || bad "carol is not listed as a seeder on tracker 1"
teardown; rm -f "$WORKDIR"/*_big.bin
fi

# ---------------------------------------------------------------------------
if want E; then
echo; echo "=== E. NETWORK PARTITION between the trackers (split brain), then heal ==="
L1=$(grep -v '^#' "$INFO" | grep -v '^$' | sed -n 1p); L2=$(grep -v '^#' "$INFO" | grep -v '^$' | sed -n 2p)
P1=${L1##*:}; P2=${L2##*:}; IP1=${L1%%:*}; IP2=${L2%%:*}
# T1 dials T2 through proxy A (port P2+100); T2 dials T1 through proxy B (port P1+100).
printf '%s\n%s\n' "$L1" "$IP2:$((P2+100))" > "$WORKDIR/info_t1.txt"
printf '%s\n%s\n' "$IP1:$((P1+100))" "$L2" > "$WORKDIR/info_t2.txt"
"$PROXY_BIN" $((P2+100)) "$IP2" "$P2" 2> "$WORKDIR/proxyA.log" & PA=$!; PIDS="$PIDS $PA"
"$PROXY_BIN" $((P1+100)) "$IP1" "$P1" 2> "$WORKDIR/proxyB.log" & PB=$!; PIDS="$PIDS $PB"
sleep 0.5
start_tracker 1 "$WORKDIR/info_t1.txt" t1
start_tracker 2 "$WORKDIR/info_t2.txt" t2; sleep 1.5
start_client alice 8961 "$INFO"    3        # attached to tracker 1
start_client bob   8962 "$SWAPPED" 4        # attached to tracker 2
head -c 2000000 /dev/urandom > "$WORKDIR/L.bin"; head -c 900000 /dev/urandom > "$WORKDIR/R.bin"
say 3 "create user alice pw"; say 3 "login alice pw"; say 3 "create group ga"; sleep 0.5
say 4 "create user bob pw";   say 4 "login bob pw"
for _ in 1 2 3; do say 4 "join group ga"; sleep 0.4; say 3 "accept request ga bob"; sleep 0.4; done
N=$(list_of 4 "$WORKDIR/bob.out" "list groups" | tr '\n' ' ')
[[ "$N" == *ga* ]] && ok "healthy link: bob (tracker 2) sees the group alice created on tracker 1" || bad "no replication even before the partition ($N)"

echo "  -- cutting the tracker<->tracker link --"
kill -USR1 "$PA" "$PB"; sleep 5
say 3 "create group g_left";  sleep 0.3; say 3 "upload file g_left $WORKDIR/L.bin"
say 4 "create group g_right"; sleep 0.3; say 4 "upload file g_right $WORKDIR/R.bin"
printf 'create user sam pwLEFT\nlogout\n'  | "$CLIENT_BIN" 127.0.0.1:8971 "$INFO"    >/dev/null 2>&1   # tracker 1
printf 'create user sam pwRIGHT\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8972 "$SWAPPED" >/dev/null 2>&1   # tracker 2, SAME name
printf 'create user dora pw\nlogout\n'     | "$CLIENT_BIN" 127.0.0.1:8973 "$SWAPPED" >/dev/null 2>&1   # only on tracker 2
sleep 1
LEFT=$(list_of 3 "$WORKDIR/alice.out" "list groups" | tr '\n' ' '); RIGHT=$(list_of 4 "$WORKDIR/bob.out" "list groups" | tr '\n' ' ')
[[ "$LEFT" == *g_left* && "$LEFT" != *g_right* ]]  && ok "partitioned: tracker 1 has g_left but not g_right ($LEFT)"  || bad "tracker 1 view wrong: $LEFT"
[[ "$RIGHT" == *g_right* && "$RIGHT" != *g_left* ]] && ok "partitioned: tracker 2 has g_right but not g_left ($RIGHT)" || bad "tracker 2 view wrong: $RIGHT"
grep -q "lost connection\|unreachable" "$WORKDIR/t1.log" && ok "tracker 1 noticed the link was down" || bad "tracker 1 did not notice the partition"

echo "  -- healing the link --"
kill -USR2 "$PA" "$PB"
for _ in $(seq 1 200); do
    A=$(list_of 3 "$WORKDIR/alice.out" "list groups" | tr '\n' ' '); B=$(list_of 4 "$WORKDIR/bob.out" "list groups" | tr '\n' ' ')
    [[ "$A" == *g_right* && "$B" == *g_left* ]] && break; sleep 0.3
done
[ "$A" = "$B" ] && [[ "$A" == *ga* && "$A" == *g_left* && "$A" == *g_right* ]] \
    && ok "converged: both trackers list ga, g_left and g_right" || bad "did not converge: T1=[$A] T2=[$B]"
# users created on either side during the partition
OUT=$(printf 'login dora pw\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8974 "$INFO" 2>&1)
echo "$OUT" | grep -q '^OK' && ok "dora (created on tracker 2 during the partition) can log in on tracker 1" || bad "dora is unknown on tracker 1"
for T in "$INFO" "$SWAPPED"; do
    W=$(printf 'login sam pwLEFT\nlogout\n'  | "$CLIENT_BIN" 127.0.0.1:8975 "$T" 2>&1 | grep -c '^OK')
    L=$(printf 'login sam pwRIGHT\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8976 "$T" 2>&1 | grep -c '^ERROR')
    [ "$W" -ge 1 ] && [ "$L" -ge 1 ] && ok "same-name conflict 'sam' resolved identically on $( [ "$T" = "$INFO" ] && echo tracker-1-first || echo tracker-2-first ): the lower-numbered tracker's account wins" \
        || bad "conflict for 'sam' not resolved consistently (left ok=$W, right rejected=$L)"
done
# and the data plane works across trackers again
say 4 "join group g_left"
for _ in 1 2 3 4 5; do say 3 "accept request g_left bob"; sleep 0.5; done
for _ in $(seq 1 30); do grep -q "Download started: \[g_left\] L.bin" "$WORKDIR/bob.out" && break
    say 4 "download file g_left L.bin $WORKDIR/bob_L.bin"; sleep 0.7; done
wait_for "$WORKDIR/bob.out" "^\[C\] \[g_left\] L.bin" 200 && [ "$(sha_of "$WORKDIR/L.bin")" = "$(sha_of "$WORKDIR/bob_L.bin")" ] \
    && ok "after healing, bob (tracker 2) downloaded alice's (tracker 1) file, byte-identical" || bad "cross-tracker download after healing failed"
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.out "$WORKDIR"/*.log && bad "crash" || ok "no crashes"
teardown
fi

# ---------------------------------------------------------------------------
if want F; then
echo; echo "=== F. a tracker is FROZEN (SIGSTOP = blackhole), then thawed ==="
start_tracker 1 "$INFO" t1
start_tracker 2 "$INFO" t2; T2=$LAST_PID; sleep 1.2
start_client alice 8981 "$INFO"    3
start_client bob   8982 "$SWAPPED" 4        # bob's preferred tracker is 2
say 3 "create user alice pw"; say 3 "login alice pw"; say 4 "create user bob pw"; say 4 "login bob pw"
say 3 "create group gf0"; sleep 1.5      # a non-empty group list, so "list groups" answers "OK: gf0"
kill -STOP "$T2"; ok "tracker 2 frozen: it accepts nothing and answers nothing"
T0=$(date +%s)
B4=$(grep -c '^OK: ' "$WORKDIR/bob.out"); say 4 "list groups"
for _ in $(seq 1 300); do [ "$(grep -c '^OK: ' "$WORKDIR/bob.out")" -gt "$B4" ] && break; sleep 0.1; done
DT=$(( $(date +%s) - T0 ))
[ "$(grep -c '^OK: ' "$WORKDIR/bob.out")" -gt "$B4" ] && ok "bob's request was answered after failing over to tracker 1 (took ${DT}s)" || bad "bob never got an answer while tracker 2 was frozen"
grep -q "failover" "$WORKDIR/bob.out" && ok "bob's client reported the failover" || bad "no failover message from bob"
say 3 "create group gf"; sleep 1
kill -CONT "$T2"; ok "tracker 2 thawed"
GOT=""
for _ in $(seq 1 100); do
    GOT=$(printf 'login alice pw\nlist groups\nlogout\n' | "$CLIENT_BIN" 127.0.0.1:8983 "$SWAPPED" 2>&1 | grep '^OK: ' | tail -1)
    [[ "$GOT" == *gf* ]] && break; sleep 0.5
done
[[ "$GOT" == *gf* ]] && ok "after thawing, tracker 2 learned the group created on tracker 1 meanwhile" || bad "tracker 2 never caught up (got: $GOT)"
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.out "$WORKDIR"/*.log && bad "crash" || ok "no crashes"
teardown
fi

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
