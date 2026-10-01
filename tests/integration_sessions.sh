#!/bin/bash
# Integration test: session semantics of `logout` and `quit`.
#
# The spec says: "logout - End current session and stop sharing files".
#   1. `logout` must NOT terminate the client: the same process can log in again,
#      even as a different user.
#   2. `logout` must withdraw every shared file from the tracker, so nobody is
#      sent to a client that no longer serves it.
#   3. `quit` (and end of input) also withdraws files and then exits.
#
# Usage: bash tests/integration_sessions.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS=0; FAIL=0; TP=""
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
trap 'kill $TP 2>/dev/null; wait 2>/dev/null; rm -rf "$WORKDIR"' EXIT

head -c 700000 /dev/urandom > "$WORKDIR/x.bin"
"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/tracker.log" 2>&1 < /dev/null & TP=$!
sleep 0.6

echo "=== one client process, several sessions ==="
OUT=$(printf 'create user alice p\nlogin alice p\ncreate group g\nupload file g %s/x.bin\nlogout\nlogin alice p\nlist files g\ncreate user zed q\nlogin zed q\nlogout\nquit\n' "$WORKDIR" \
      | "$CLIENT_BIN" 127.0.0.1:9401 "$INFO" 2>&1)
echo "$OUT" | grep -q "^OK: x.bin" && ok "after logout the same process logged in again (alice) and still saw the group's file" || bad "could not log in again after logout"
[ "$(echo "$OUT" | grep -c '^Exiting client')" -eq 1 ] && ok "client exited only on quit" || bad "client did not exit exactly once"
echo "$OUT" | grep -q "ERROR" && bad "unexpected ERROR in a normal session sequence" || ok "no errors: login as a second user (zed) in the same process worked"

echo "=== logout withdraws the shared file ==="
printf 'create user bob p\nlogin bob p\njoin group g\nquit\n' | "$CLIENT_BIN" 127.0.0.1:9402 "$INFO" >/dev/null 2>&1
printf 'login alice p\naccept request g bob\nquit\n'          | "$CLIENT_BIN" 127.0.0.1:9403 "$INFO" >/dev/null 2>&1
OUT=$(printf 'login bob p\ndownload file g x.bin %s/got.bin\nquit\n' "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:9404 "$INFO" 2>&1)
echo "$OUT" | grep -q "No seeders available" && ok "alice logged out earlier: her file is no longer offered to bob" || bad "bob was still pointed at a client that stopped sharing"

echo "=== a live seeder is offered; quit withdraws it ==="
mkfifo "$WORKDIR/a.fifo"
"$CLIENT_BIN" 127.0.0.1:9405 "$INFO" < "$WORKDIR/a.fifo" > "$WORKDIR/a.out" 2>&1 & AP=$!
exec 3>"$WORKDIR/a.fifo"
printf 'login alice p\nupload file g %s/x.bin\n' "$WORKDIR" >&3
sleep 1.5
OUT=$(printf 'login bob p\ndownload file g x.bin %s/got1.bin\nquit\n' "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:9406 "$INFO" 2>&1)
cmp -s "$WORKDIR/x.bin" "$WORKDIR/got1.bin" && ok "while alice is logged in, bob downloads the file" || bad "bob could not download from a live seeder"
printf 'quit\n' >&3; for _ in $(seq 1 50); do kill -0 $AP 2>/dev/null || break; sleep 0.1; done
kill -0 $AP 2>/dev/null && bad "alice's client did not exit on quit" || ok "alice's client exits on quit"
exec 3>&-
OUT=$(printf 'login bob p\ndownload file g x.bin %s/got2.bin\nquit\n' "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:9407 "$INFO" 2>&1)
echo "$OUT" | grep -q "No seeders available" && ok "after alice quit, the tracker no longer lists her as a seeder" || bad "tracker still offers alice after quit"
echo "=== the destination may be a directory ==="
mkdir "$WORKDIR/outdir"
"$CLIENT_BIN" 127.0.0.1:9408 "$INFO" < <(printf 'login alice p\nupload file g %s/x.bin\n' "$WORKDIR"; sleep 2.5) > /dev/null 2>&1 &
AP2=$!
sleep 1.2
OUT=$(printf 'login bob p\ndownload file g x.bin %s\n' "$WORKDIR/outdir" | "$CLIENT_BIN" 127.0.0.1:9409 "$INFO" 2>&1)
cmp -s "$WORKDIR/x.bin" "$WORKDIR/outdir/x.bin" && ok "download into an existing directory saved it as <dir>/x.bin" || bad "directory destination did not work"
OUT=$(printf 'login bob p\ndownload file g x.bin %s/nonexistent/\nquit\n' "$WORKDIR" | "$CLIENT_BIN" 127.0.0.1:9410 "$INFO" 2>&1)
echo "$OUT" | grep -q "does not exist" && ok "a trailing-slash destination that is not a directory is refused with a clear message" || bad "no clear error for a missing directory"
wait $AP2 2>/dev/null
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.log "$WORKDIR"/a.out && bad "crash" || ok "no crashes"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
