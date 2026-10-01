#!/bin/bash
# Full-scale concurrency test -- 3 clients + 2 trackers, all
# operating at the same time (NOT a scripted step-by-step sequence).
#
#   alice  -> tracker 1 (first in her tracker file)
#   bob    -> tracker 2 (her file is swapped, so tracker 2 is preferred)
#   carol  -> tracker 1
#
# Each of the three runs its own driver in the background, with random jitter
# between commands, so the following genuinely overlap in time:
#   - account + group creation on different trackers
#   - join requests / accepts crossing between trackers
#   - uploads of files owned by different users
#   - all SIX cross-downloads (each user downloads the other two's file)
#   - `list groups` / `list files` traffic while the transfers are running
#
# Pass criteria: every download completes and is byte-identical to the
# original, every process survives, and afterwards BOTH trackers report exactly
# the same groups and files (i.e. replication converged).
#
# Usage: bash tests/stress_concurrency.sh     (optionally: SEED=42 to repeat a run)

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=${WORKDIR:-$(mktemp -d)}
mkdir -p "$WORKDIR"
SWAPPED="$WORKDIR/tracker_info_swapped.txt"
grep -v '^#' "$INFO" | tac > "$SWAPPED"
RANDOM=${SEED:-$$}
PASS=0; FAIL=0
PIDS=""
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    [ -z "${KEEP:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

jitter() { sleep "0.$((RANDOM % 4))$((RANDOM % 10))"; }
send()   { printf '%s\n' "$2" >&"$1"; }          # send FD "command"

# Re-send a command every 0.7 s until PATTERN shows up in the client's output.
retry_until() {  # fd outfile pattern command
    for _ in $(seq 1 100); do
        grep -q -- "$3" "$2" 2>/dev/null && return 0
        send "$1" "$4"; sleep 0.7
    done
    return 1
}

start_client() {  # name port infofile
    mkfifo "$WORKDIR/$1.fifo"
    "$CLIENT_BIN" "127.0.0.1:$2" "$3" < "$WORKDIR/$1.fifo" > "$WORKDIR/$1.out" 2>&1 &
    PIDS="$PIDS $!"
}

echo "=== Creating test files ==="
head -c $((8*1024*1024 + 1234)) /dev/urandom > "$WORKDIR/A1.bin"
head -c 700000                  /dev/urandom > "$WORKDIR/A2.bin"
head -c $((6*1024*1024 + 77))   /dev/urandom > "$WORKDIR/B1.bin"
head -c $((5*1024*1024 + 999))  /dev/urandom > "$WORKDIR/C1.bin"

echo "=== Starting 2 trackers + 3 clients ==="
"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/tracker1.log" 2>&1 < /dev/null & PIDS="$PIDS $!"
"$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/tracker2.log" 2>&1 < /dev/null & PIDS="$PIDS $!"
sleep 1
start_client alice 8801 "$INFO"
start_client bob   8802 "$SWAPPED"
start_client carol 8803 "$INFO"
exec 3>"$WORKDIR/alice.fifo" 4>"$WORKDIR/bob.fifo" 5>"$WORKDIR/carol.fifo"
sleep 0.5

# --------------------------------------------------------------------------
# Each driver: (fd, me, my group, my big file, the two OTHER groups+files)
# --------------------------------------------------------------------------
driver() {
    local fd=$1 me=$2 mygrp=$3 upl=$4 g1=$5 f1=$6 g2=$7 f2=$8 out="$WORKDIR/$2.out"
    local others=""; case $me in alice) others="bob carol";; bob) others="alice carol";; carol) others="alice bob";; esac
    send $fd "create user $me pw_$me"; jitter
    send $fd "login $me pw_$me";       jitter
    send $fd "create group $mygrp";    jitter
    send $fd "upload file $mygrp $WORKDIR/$upl"; jitter
    [ "$me" = alice ] && { send $fd "upload file ga $WORKDIR/A2.bin"; jitter; }
    # Membership dance: ask to join the others' groups and accept their requests,
    # repeatedly, while also hammering the read-only commands. Duplicates just
    # come back as harmless ERRORs.
    for _ in $(seq 1 14); do
        for o in $others; do send $fd "accept request $mygrp $o"; done
        send $fd "join group $g1"; send $fd "join group $g2"
        send $fd "list groups";    send $fd "list files $mygrp"
        jitter
    done
    retry_until $fd "$out" "Download started: \[$g1\] $f1" "download file $g1 $f1 $WORKDIR/${me}_got_$f1"
    retry_until $fd "$out" "Download started: \[$g2\] $f2" "download file $g2 $f2 $WORKDIR/${me}_got_$f2"
    for _ in 1 2 3; do send $fd "show downloads"; send $fd "list files $g1"; jitter; done
}

echo "=== Running the three drivers CONCURRENTLY ==="
T0=$(date +%s)
driver 3 alice ga A1.bin gb B1.bin gc C1.bin &  D1=$!
driver 4 bob   gb B1.bin ga A1.bin gc C1.bin &  D2=$!
driver 5 carol gc C1.bin ga A1.bin gb B1.bin &  D3=$!
wait $D1 $D2 $D3
echo "    drivers finished issuing commands after $(( $(date +%s) - T0 ))s; waiting for transfers..."

# Wait for all 6 cross-downloads (2 per client) to be reported complete.
# Count DISTINCT [C] lines per client: `show downloads` repeats them, and two
# different clients legitimately complete the same file.
for _ in $(seq 1 1200); do
    n=0
    for c in alice bob carol; do n=$((n + $(grep '^\[C\] ' "$WORKDIR/$c.out" | sort -u | wc -l))); done
    [ "$n" -ge 6 ] && break; sleep 0.1
done
echo "    total wall time: $(( $(date +%s) - T0 ))s"

echo "=== Checking transfers ==="
verify() { # downloader group file
    local got="$WORKDIR/$1_got_$3"
    [ "$(sha1sum < "$WORKDIR/$3" 2>/dev/null)" = "$(sha1sum < "$got" 2>/dev/null)" ] \
        && ok "$1 got $3 (from group $2), SHA1 matches" || bad "$1 did NOT get an identical $3"
}
verify alice gb B1.bin; verify alice gc C1.bin
verify bob   ga A1.bin; verify bob   gc C1.bin
verify carol ga A1.bin; verify carol gb B1.bin
grep -q "Warning: download succeeded" "$WORKDIR"/{alice,bob,carol}.out && bad "a seeder announcement was rejected" || ok "all downloads registered their client as a seeder"

echo "=== Checking that BOTH trackers agree (replication converged) ==="
sleep 1.5
audit() {  # infofile port outfile
    printf 'login alice pw_alice\nlist groups\nlist files ga\nlist files gb\nlist files gc\nlogout\n' \
        | "$CLIENT_BIN" "127.0.0.1:$2" "$1" 2>&1 \
        | grep -E '^OK: ' | tr '|' '\n' | sed 's/^OK: //' | sort > "$3"
}
audit "$INFO"    8811 "$WORKDIR/audit_t1.txt"
audit "$SWAPPED" 8812 "$WORKDIR/audit_t2.txt"
if [ -s "$WORKDIR/audit_t1.txt" ] && diff -q "$WORKDIR/audit_t1.txt" "$WORKDIR/audit_t2.txt" >/dev/null; then
    ok "tracker 1 and tracker 2 report identical groups and files ($(wc -l < "$WORKDIR/audit_t1.txt") entries)"
else
    bad "trackers DISAGREE:"; diff "$WORKDIR/audit_t1.txt" "$WORKDIR/audit_t2.txt" | head
fi
for want in ga gb gc A1.bin A2.bin B1.bin C1.bin; do
    grep -qx "$want" "$WORKDIR/audit_t1.txt" && ok "tracker state contains $want" || bad "tracker state is missing $want"
done

echo "=== Health ==="
grep -qi "segmentation\|core dumped\|terminate called\|AddressSanitizer\|ThreadSanitizer" "$WORKDIR"/*.log "$WORKDIR"/*.out \
    && bad "a process crashed" || ok "no crashes in any tracker or client"
for p in $PIDS; do kill -0 "$p" 2>/dev/null || bad "process $p died during the test"; done

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
