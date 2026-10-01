#!/bin/bash
# File-type and file-size coverage.
#
# alice uploads a set of REAL files; bob downloads them all from alice
# (single-peer path), then carol downloads them all from alice + bob
# (multi-peer path). Every download must be byte-identical (cmp), and the
# formats are additionally checked with their own tools where they exist
# (tar, unzip, the ELF binary is actually executed, the PNG is parsed).
#
#   text (UTF-8)   real ELF executable   PNG image   tar.gz   zip
#   sizes around the 512 KiB piece boundary:  0 B, 1 B, 512K-1, 512K, 512K+1, 1M
#   awkward content: a file made only of '|' and NUL bytes (the control-message
#   delimiter must never matter for piece data)
#
# Usage: bash tests/file_types.sh

set -u
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TRACKER_BIN="$PROJECT_DIR/tracker/tracker"
CLIENT_BIN="$PROJECT_DIR/client/client"
INFO="$PROJECT_DIR/tracker_info.txt"
WORKDIR=${WORKDIR:-$(mktemp -d)}
mkdir -p "$WORKDIR/src" "$WORKDIR/bob" "$WORKDIR/carol"
PASS=0; FAIL=0; PIDS=""
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    [ -z "${KEEP:-}" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT
wait_for() { for _ in $(seq 1 ${3:-300}); do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }

S="$WORKDIR/src"
echo "=== Generating test files ==="
# text: UTF-8 with multi-byte characters, long lines, ~1.3 MB
python3 - "$S/notes.txt" <<'EOF'
import sys
with open(sys.argv[1], "w", encoding="utf-8") as f:
    for i in range(20000):
        f.write(f"line {i}: naïve café – 你好, мир! Ωmega | pipe | tab\there \u2603\n")
EOF
cp /bin/ls "$S/program.elf"                                  # a real executable
python3 - "$S/image.png" <<'EOF'                             # a real 800x600 RGB PNG
import sys, zlib, struct, random
w, h = 800, 600
random.seed(1)
raw = b"".join(b"\x00" + bytes((x*y + random.randrange(40)) % 256 for x in range(w) for _ in range(3)) for y in range(h))
def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))
EOF
tar -czf "$S/archive.tar.gz" -C /usr/include stdio.h stdlib.h string.h math.h 2>/dev/null
( cd "$S" && zip -q archive.zip notes.txt image.png )
P=$((512*1024))
: > "$S/empty.bin"
printf 'Z' > "$S/one_byte.bin"
head -c $((P-1)) /dev/urandom > "$S/p_minus_1.bin"
head -c $P       /dev/urandom > "$S/exactly_1_piece.bin"
head -c $((P+1)) /dev/urandom > "$S/p_plus_1.bin"
head -c $((P*2)) /dev/urandom > "$S/exactly_2_pieces.bin"
python3 -c "import sys; sys.stdout.buffer.write((b'|' * 700 + b'\x00' * 700 + b'|\n|\x00') * 900)" > "$S/delims_and_nuls.bin"
FILES=$(cd "$S" && ls)
echo "    $(echo $FILES | wc -w) files, $(du -sh "$S" | cut -f1) total"

"$TRACKER_BIN" "$INFO" 1 > "$WORKDIR/tracker.log" 2>&1 < /dev/null & PIDS="$PIDS $!"
sleep 0.6
for spec in "alice 8991 3" "bob 8992 4" "carol 8993 5"; do
    set -- $spec
    mkfifo "$WORKDIR/$1.fifo"
    "$CLIENT_BIN" "127.0.0.1:$2" "$INFO" < "$WORKDIR/$1.fifo" > "$WORKDIR/$1.out" 2>&1 & PIDS="$PIDS $!"
    eval "exec $3>\"$WORKDIR/$1.fifo\""
done
say() { printf '%s\n' "$2" >&"$1"; }
say 3 "create user alice pw"; say 3 "login alice pw"; say 3 "create group g"
say 4 "create user bob pw";   say 4 "login bob pw"
say 5 "create user carol pw"; say 5 "login carol pw"; sleep 0.8
for _ in 1 2 3; do say 4 "join group g"; say 5 "join group g"; sleep 0.3; say 3 "accept request g bob"; say 3 "accept request g carol"; sleep 0.3; done

echo "=== alice uploads every file ==="
for f in $FILES; do say 3 "upload file g $S/$f"; done
sleep 4
say 3 "list files g"; sleep 0.8
LISTED=$(grep '^OK: ' "$WORKDIR/alice.out" | tail -1 | sed 's/^OK: //' | tr '|' '\n' | sort | tr '\n' ' ')
WANT=$(echo $FILES | tr ' ' '\n' | sort | tr '\n' ' ')
[ "$LISTED" = "$WANT" ] && ok "all $(echo $FILES | wc -w) files were accepted by the tracker (including the empty one)" || bad "tracker lists [$LISTED], expected [$WANT]"

download_all() {  # fd who dir
    for f in $FILES; do say $1 "download file g $f $3/$f"; done
    for f in $FILES; do
        wait_for "$WORKDIR/$2.out" "^\[C\] \[g\] $f\$" 250 || echo "    (no [C] line for $f)"
    done
    sleep 1
}
check_all() {  # who dir
    for f in $FILES; do
        if cmp -s "$S/$f" "$2/$f"; then ok "$1: $f is byte-identical ($(stat -c %s "$S/$f") bytes)"
        else bad "$1: $f differs or is missing ($(stat -c %s "$2/$f" 2>/dev/null || echo missing) bytes, want $(stat -c %s "$S/$f"))"; fi
    done
}
echo "=== bob downloads everything from alice (single-peer path) ==="
download_all 4 bob "$WORKDIR/bob"; check_all bob "$WORKDIR/bob"
echo "=== carol downloads everything from alice + bob (multi-peer path) ==="
sleep 2
download_all 5 carol "$WORKDIR/carol"; check_all carol "$WORKDIR/carol"
[ "$(grep -c 'from 2 peers' "$WORKDIR/carol.out")" -ge 1 ] && ok "carol used the multi-peer path" || bad "carol never used the multi-peer path"

echo "=== The formats still WORK after the round trip ==="
D="$WORKDIR/carol"
tar -tzf "$D/archive.tar.gz" >/dev/null 2>&1 && ok "downloaded tar.gz lists and decompresses" || bad "tar.gz is damaged"
unzip -tq "$D/archive.zip" >/dev/null 2>&1 && ok "downloaded zip passes 'unzip -t'" || bad "zip is damaged"
chmod +x "$D/program.elf"; "$D/program.elf" --version >/dev/null 2>&1 && ok "downloaded ELF binary actually runs" || bad "ELF binary does not run"
python3 - "$D/image.png" <<'EOF' && ok "downloaded PNG parses (signature, chunk CRCs, pixel data)" || bad "PNG is damaged"
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"\x89PNG\r\n\x1a\n"
i = 8; idat = b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i+8]); body = d[i+8:i+8+n]
    assert struct.unpack(">I", d[i+8+n:i+12+n])[0] == zlib.crc32(t + body) & 0xffffffff
    if t == b"IDAT": idat += body
    i += 12 + n
assert len(zlib.decompress(idat)) == 600 * (1 + 800 * 3)
EOF
grep -q "Warning: download succeeded" "$WORKDIR"/bob.out "$WORKDIR"/carol.out && bad "a seeder announcement was rejected" || ok "every download registered its client as a seeder"
grep -qi "segmentation\|core dumped\|terminate called" "$WORKDIR"/*.out "$WORKDIR/tracker.log" && bad "a process crashed" || ok "no crashes"

echo; echo "RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
