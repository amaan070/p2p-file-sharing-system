#!/bin/bash
# Integration test: real tracker + two real clients, uploading and
# downloading files across several size boundaries (small, exactly one
# piece, multi-piece with a smaller final piece), verifying byte-for-byte
# correctness and hash matches for each.

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
TRACKER_INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=$(mktemp -d)
PASS_COUNT=0
FAIL_COUNT=0

cleanup() {
    exec 3>&- 2>/dev/null
    kill "$TRACKER_PID" 2>/dev/null
    kill "$ALICE_PID" 2>/dev/null
    wait "$TRACKER_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

check() {
    if [ "$1" = "$2" ]; then
        echo "  PASS: $3"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $3 (expected '$2', got '$1')"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

echo "=== Starting tracker ==="
"$TRACKER_BIN" "$TRACKER_INFO" 1 > "$WORKDIR/tracker.log" 2>&1 &
TRACKER_PID=$!
sleep 0.5

# Create test files of three distinct size categories.
PIECE_SIZE=$((512 * 1024))

echo "=== Creating test files ==="
# 1. Small file, well under one piece.
head -c 1000 /dev/urandom > "$WORKDIR/small.bin"
echo "  small.bin: 1000 bytes (<1 piece)"

# 2. Exactly one piece boundary.
head -c "$PIECE_SIZE" /dev/urandom > "$WORKDIR/exact_piece.bin"
echo "  exact_piece.bin: $PIECE_SIZE bytes (exactly 1 piece)"

# 3. Multi-piece with a smaller final piece.
MULTI_SIZE=$((PIECE_SIZE * 2 + 12345))
head -c "$MULTI_SIZE" /dev/urandom > "$WORKDIR/multi_piece.bin"
echo "  multi_piece.bin: $MULTI_SIZE bytes (2 full pieces + partial)"

FILES=("small.bin" "exact_piece.bin" "multi_piece.bin")

echo ""
echo "=== Running alice (uploader) — kept alive via FIFO so her seeder stays up ==="
ALICE_FIFO="$WORKDIR/alice_fifo"
mkfifo "$ALICE_FIFO"

"$CLIENT_BIN" 127.0.0.1:8001 "$TRACKER_INFO" < "$ALICE_FIFO" > "$WORKDIR/alice_out.txt" 2>&1 &
ALICE_PID=$!

# Open the FIFO's write end on fd 3 and keep it open. This is the key fix:
# as long as SOME writer has the FIFO open, the client's read on stdin
# blocks for more input instead of hitting EOF and exiting -- exactly like
# a real interactive terminal session that hasn't been closed.
exec 3>"$ALICE_FIFO"

cat << EOF >&3
create user alice pw123
login alice pw123
create group cs101
upload file cs101 $WORKDIR/small.bin
upload file cs101 $WORKDIR/exact_piece.bin
upload file cs101 $WORKDIR/multi_piece.bin
list files cs101
EOF

sleep 1

echo "--- alice output (so far) ---"
cat "$WORKDIR/alice_out.txt"
echo "--- end alice output ---"
echo ""

echo "=== bob joins the group (separate short-lived session -- fine, bob isn't a seeder yet) ==="
BOB_CMDS="$WORKDIR/bob_cmds.txt"
cat > "$BOB_CMDS" << EOF
create user bob pw456
login bob pw456
join group cs101
logout
EOF

"$CLIENT_BIN" 127.0.0.1:8002 "$TRACKER_INFO" < "$BOB_CMDS" > "$WORKDIR/bob_join_out.txt" 2>&1
sleep 0.5

echo "--- bob join output ---"
cat "$WORKDIR/bob_join_out.txt"
echo ""

echo "=== alice accepts bob's join request (same continuous session, via her FIFO) ==="
cat << EOF >&3
accept request cs101 bob
EOF
sleep 0.5

echo "--- alice output (after accept) ---"
cat "$WORKDIR/alice_out.txt"
echo ""

echo "=== bob downloads all three files ==="
BOB_DL_CMDS="$WORKDIR/bob_dl_cmds.txt"
cat > "$BOB_DL_CMDS" << EOF
login bob pw456
download file cs101 small.bin $WORKDIR/downloaded_small.bin
download file cs101 exact_piece.bin $WORKDIR/downloaded_exact_piece.bin
download file cs101 multi_piece.bin $WORKDIR/downloaded_multi_piece.bin
logout
EOF

"$CLIENT_BIN" 127.0.0.1:8003 "$TRACKER_INFO" < "$BOB_DL_CMDS" > "$WORKDIR/bob_dl_out.txt" 2>&1
sleep 0.5

echo "--- bob download output ---"
cat "$WORKDIR/bob_dl_out.txt"
echo ""

echo "=== Verifying byte-for-byte correctness and hashes ==="
for f in "${FILES[@]}"; do
    orig="$WORKDIR/$f"
    dl="$WORKDIR/downloaded_$f"

    if [ ! -f "$dl" ]; then
        echo "  FAIL: $f was not downloaded (file missing)"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue
    fi

    orig_size=$(stat -c%s "$orig")
    dl_size=$(stat -c%s "$dl")
    check "$dl_size" "$orig_size" "$f: downloaded size matches original ($orig_size bytes)"

    if diff -q "$orig" "$dl" > /dev/null 2>&1; then
        echo "  PASS: $f is byte-for-byte identical to original"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $f differs from original"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    orig_hash=$(sha1sum "$orig" | cut -d' ' -f1)
    dl_hash=$(sha1sum "$dl" | cut -d' ' -f1)
    check "$dl_hash" "$orig_hash" "$f: SHA1 hash matches ($orig_hash)"

    # Confirm the "[C]" completion line appeared for this file in bob's output.
    if grep -q "\[C\] \[cs101\] $f" "$WORKDIR/bob_dl_out.txt"; then
        echo "  PASS: $f showed correct [C] completion status line"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $f did not show the expected [C] completion line"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
    echo ""
done

echo "=== Checking tracker log for crashes/errors ==="
if grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR/tracker.log"; then
    echo "  FAIL: tracker log shows a crash"
    FAIL_COUNT=$((FAIL_COUNT + 1))
else
    echo "  PASS: tracker ran throughout with no crash indicators"
    PASS_COUNT=$((PASS_COUNT + 1))
fi

echo ""
echo "================================"
echo "RESULTS: $PASS_COUNT passed, $FAIL_COUNT failed"
echo "================================"

if [ "$FAIL_COUNT" -eq 0 ]; then
    exit 0
else
    exit 1
fi
