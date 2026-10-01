#!/bin/bash
# Integration test: client failover + rolling tracker crashes
# + concurrent load. 2 trackers, long-running clients that are NEVER restarted.
#
#   A. alice's tracker (T2) crashes. alice's client must notice by itself
#      (she types nothing), switch to T1 and log herself back in, so other
#      users can still download from her.
#   B. T2 restarts with EMPTY state and must be refilled from T1
#      (including a group created while it was down).
#   C. T1 -- where alice and bob now live -- crashes. Both clients must fail
#      over to the restarted T2. Their re-login only works if T2 really
#      recovered their accounts, so this proves the resync.
#   D. T1 restarts empty and is refilled from T2 (including a group created
#      on T2 while T1 was down).
#   E. Concurrency: 3 clients hammer both trackers at once; afterwards both
#      trackers must report exactly the same set of groups.
# Run from anywhere:  bash tests/integration_failover.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS=0; FAIL=0
T1_PID=""; T2_PID=""; ALICE_PID=""; BOB_PID=""

SWAPPED="$WORKDIR/tracker_info_swapped.txt"   # clients using this attach to tracker 2 first
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
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }
# wait_count <file> <pattern> <min_count> [tenths_of_second_timeout]
wait_count() { for _ in $(seq 1 ${4:-150}); do [ "$(count "$1" "$2")" -ge "$3" ] && return 0; sleep 0.1; done; return 1; }
run_client() { "$CLIENT_BIN" 127.0.0.1:"$1" "$2" > "$3" 2>&1; } # commands on stdin
same_sha() { [ "$(sha1sum < "$1")" = "$(sha1sum < "$2" 2>/dev/null)" ]; }

head -c $((512*1024*3 + 4242)) /dev/urandom > "$WORKDIR/A.bin"

start_t1() { "$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/t1_$1.log" 2>&1 < /dev/null & T1_PID=$!; }
start_t2() { "$TRACKER_BIN" "$INFO" 2 > "$WORKDIR/t2_$1.log" 2>&1 < /dev/null & T2_PID=$!; }
start_t1 run1; start_t2 run1
wait_count "$WORKDIR/t1_run1.log" "connected to peer" 1 60 && wait_count "$WORKDIR/t2_run1.log" "connected to peer" 1 60 \
    && ok "trackers connected to each other" || bad "trackers did not connect"

echo "=== Setup: alice on T2, bob on T1, both long-running ==="
mkfifo "$WORKDIR/alice_fifo" "$WORKDIR/bob_fifo"
"$CLIENT_BIN" 127.0.0.1:8401 "$SWAPPED" < "$WORKDIR/alice_fifo" > "$WORKDIR/alice.out" 2>&1 & ALICE_PID=$!
exec 3>"$WORKDIR/alice_fifo"
printf 'create user alice pw1\nlogin alice pw1\ncreate group g1\nupload file g1 %s/A.bin\n' "$WORKDIR" >&3
sleep 1
"$CLIENT_BIN" 127.0.0.1:8402 "$INFO" < "$WORKDIR/bob_fifo" > "$WORKDIR/bob.out" 2>&1 & BOB_PID=$!
exec 4>"$WORKDIR/bob_fifo"
printf 'create user bob pw2\nlogin bob pw2\njoin group g1\n' >&4
printf 'create user dave pw4\n' | run_client 8499 "$INFO" /dev/null
sleep 0.7
printf 'accept request g1 bob\n' >&3
sleep 0.7
grep -q "Connected to tracker at 127.0.0.1:9002" "$WORKDIR/alice.out" && grep -q "Connected to tracker at 127.0.0.1:9001" "$WORKDIR/bob.out" \
    && ok "alice is on T2, bob is on T1" || bad "clients not attached as intended"

echo "=== A. T2 (alice's tracker) crashes; alice must recover on her own ==="
kill -9 "$T2_PID"; wait "$T2_PID" 2>/dev/null; T2_PID=""
wait_count "$WORKDIR/alice.out" "\[failover\] switched to tracker at 127.0.0.1:9001" 1 150 \
    && ok "alice's client noticed the crash and switched to T1 without any command typed" || bad "alice did not fail over on her own"
wait_count "$WORKDIR/alice.out" "session for 'alice' restored" 1 50 \
    && ok "alice was logged back in automatically" || bad "alice's session was not restored"
printf 'download file g1 A.bin %s/bob_A.bin\n' "$WORKDIR" >&4
wait_count "$WORKDIR/bob.out" "\[C\] \[g1\] A.bin" 1 150 && same_sha "$WORKDIR/A.bin" "$WORKDIR/bob_A.bin" \
    && ok "bob downloaded from alice via T1 (alice is visible as online again; SHA1 matches)" || { bad "bob could not download from alice after the crash"; tail -3 "$WORKDIR/bob.out"; }
printf 'create group g5\n' >&3
sleep 0.7
printf 'list groups\n' >&4
sleep 0.7
grep -q "g5" "$WORKDIR/bob.out" && ok "alice's commands keep working after failover (g5 created, bob sees it)" || bad "alice's post-failover command failed"
printf 'create user erin pw5\n' | run_client 8498 "$INFO" /dev/null   # a change made while T2 is down

echo "=== B. T2 restarts with EMPTY state ==="
start_t2 run2
wait_count "$WORKDIR/t2_run2.log" "connected to peer" 1 100 && ok "restarted T2 connected to T1" || bad "restarted T2 never connected"
wait_count "$WORKDIR/t1_run1.log" "resync complete" 2 200 && ok "T1 pushed a snapshot to the restarted T2" || bad "T1 never resynced T2"
sleep 0.7
printf 'login dave pw4\nlist groups\nlogout\n' | run_client 8403 "$SWAPPED" "$WORKDIR/dave1.out"
grep -q "Connected to tracker at 127.0.0.1:9002" "$WORKDIR/dave1.out" && grep -q "g1|g5" "$WORKDIR/dave1.out" \
    && ok "restarted T2 knows dave's account and both groups (g5 was created while it was down)" || { bad "T2 did not recover state"; cat "$WORKDIR/dave1.out"; }
printf 'login erin pw5\nlogout\n' | run_client 8404 "$SWAPPED" "$WORKDIR/erin.out"
[ "$(count "$WORKDIR/erin.out" '^OK')" -eq 2 ] && ok "account created during the outage (erin) works on the restarted T2" || bad "erin cannot log in on T2"

echo "=== C. T1 (where alice AND bob now live) crashes ==="
kill -9 "$T1_PID"; wait "$T1_PID" 2>/dev/null; T1_PID=""
wait_count "$WORKDIR/alice.out" "\[failover\] switched to tracker at 127.0.0.1:9002" 1 150 && wait_count "$WORKDIR/bob.out" "\[failover\] switched to tracker at 127.0.0.1:9002" 1 150 \
    && ok "both long-running clients failed over to the restarted T2" || bad "a client did not fail over to T2"
wait_count "$WORKDIR/alice.out" "session for 'alice' restored" 2 100 && wait_count "$WORKDIR/bob.out" "session for 'bob' restored" 1 100 \
    && ok "both were re-logged in on T2 (needs their accounts to have survived the restart)" || bad "re-login on T2 failed"
printf 'download file g1 A.bin %s/bob_A2.bin\n' "$WORKDIR" >&4
wait_count "$WORKDIR/bob.out" "\[C\] \[g1\] A.bin" 2 150 && same_sha "$WORKDIR/A.bin" "$WORKDIR/bob_A2.bin" \
    && ok "bob downloaded from alice again, now entirely on T2 (SHA1 matches)" || { bad "download failed after the second crash"; tail -3 "$WORKDIR/bob.out"; }
printf 'login dave pw4\ncreate group g6\nlogout\n' | run_client 8405 "$SWAPPED" /dev/null   # a change made while T1 is down

echo "=== D. T1 restarts with EMPTY state ==="
start_t1 run2
wait_count "$WORKDIR/t1_run2.log" "connected to peer" 1 100 && ok "restarted T1 connected to T2" || bad "restarted T1 never connected"
wait_count "$WORKDIR/t2_run2.log" "resync complete" 2 200 && ok "T2 pushed a snapshot to the restarted T1" || bad "T2 never resynced T1"
sleep 0.7
printf 'login erin pw5\nlist groups\nlogout\n' | run_client 8406 "$INFO" "$WORKDIR/erin2.out"
grep -q "Connected to tracker at 127.0.0.1:9001" "$WORKDIR/erin2.out" && grep -q "g1|g5|g6" "$WORKDIR/erin2.out" \
    && ok "restarted T1 recovered everything, including g6 (created while it was down)" || { bad "T1 did not recover state"; cat "$WORKDIR/erin2.out"; }

echo "=== E. Concurrent load on both trackers ==="
for i in 1 2 3; do
    ( printf 'create user s%s pw\nlogin s%s pw\n' $i $i
      for j in $(seq 1 8); do printf 'create group s%s_%s\nlist groups\n' $i $j; done
      printf 'logout\n' ) | run_client $((8420+i)) $([ $i -eq 3 ] && echo "$SWAPPED" || echo "$INFO") "$WORKDIR/stress$i.out" &
done
wait_stress=$(jobs -p | grep -v -e "^$ALICE_PID$" -e "^$BOB_PID$" -e "^$T1_PID$" -e "^$T2_PID$"); for p in $wait_stress; do wait "$p" 2>/dev/null; done
sleep 1.5
printf 'login dave pw4\nlist groups\nlogout\n' | run_client 8431 "$INFO"    "$WORKDIR/final_t1.out"
printf 'login dave pw4\nlist groups\nlogout\n' | run_client 8432 "$SWAPPED" "$WORKDIR/final_t2.out"
G1=$(grep '^OK: ' "$WORKDIR/final_t1.out" | grep 'g1' | head -1); G2=$(grep '^OK: ' "$WORKDIR/final_t2.out" | grep 'g1' | head -1)
NGROUPS=$(echo "$G1" | tr -cd '|' | wc -c)
[ -n "$G1" ] && [ "$G1" = "$G2" ] && ok "both trackers report the identical group list after concurrent load" || { bad "trackers diverged under load"; echo "T1: $G1"; echo "T2: $G2"; }
[ "$NGROUPS" -eq 26 ] && ok "all 27 groups present on both (g1, g5, g6 + 24 created concurrently)" || bad "expected 27 groups, found $((NGROUPS+1))"

echo "=== Health ==="
kill -0 "$T1_PID" 2>/dev/null && kill -0 "$T2_PID" 2>/dev/null && ok "both trackers alive at the end" || bad "a tracker died"
kill -0 "$ALICE_PID" 2>/dev/null && kill -0 "$BOB_PID" 2>/dev/null && ok "both long-running clients survived two tracker crashes each" || bad "a client died"
echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
