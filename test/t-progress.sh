#!/usr/bin/env bash
# On a terminal, a long sync shows how far it got on a line of its own, and
# -q leaves out the file lines
. "$(dirname "$0")/lib.sh"

# -q: the summary and conflicts, not each file
p="$L"
for i in $(seq 8); do # the same on both sides, for later
        p="$p/d$i"
        mkdir -p "$p"
        echo x >"$p/f"
done
mkdir -p "$R"
cp -a "$L/." "$R/"
echo a >"$L/up"
echo b >"$R/down"
echo here >"$L/both"
echo there >"$R/both"
start ./proj host:proj -q
wait_same
grep -q '[↑↓]' "$T/out" && fail "-q listed files"
expect_out "in sync: 2 sent, 2 received"
expect_out "! both changed on both sides"
# The copy it kept aside is on both sides after that one run
[ -e "$L/both.isf-conflict" ] && [ -e "$R/both.isf-conflict" ] || fail "the conflict copy stayed on one side"
stop

# Comparing a deep tree over a slow link (a dry run: it exits by itself)
export ISF_TEST_LATENCY_MS=300
(cd "$T/local" && exec timeout 20 script -qec "exec '$ISF' -n ./proj -I '$ISF'" /dev/null >"$T/out" 2>"$T/err")
grep -qaE 'isf: comparing, [0-9]+ folders listed'$'\r\e\\[Kisf: comparing, [0-9]+ folders listed' "$T/out" ||
        fail "no status line while comparing"
grep -qaE $'\r\e\\[Kisf: already in sync' "$T/out" || fail "the status line wasn't cleared"

# A slow sync on a terminal (script gives isf one): a status line, rewritten in
# place, that goes before the summary
rm -rf "$L/d1" "$R/d1"
head -c 10000000 /dev/urandom >"$L/big"
export ISF_TEST_LATENCY_MS=200
(cd "$T/local" && exec setsid script -qfec "exec '$ISF' ./proj -I '$ISF'" /dev/null >"$T/out" 2>"$T/err") &
PID=$!
WAIT=20 wait_for 'grep -q "watching for changes" "$T/out"'
grep -qaE $'\r\e\\[Kisf: 0 of 1 file, [0-9.]+ of 9.5 MB' "$T/out" || fail "no status line"
grep -qaE $'\r\e\\[K  ↑ big' "$T/out" || fail "the status line wasn't cleared"
grep -zqaE '↑ big[[:space:]]+isf: 1 of 1 file,' "$T/out" || fail "the status line under it didn't count it"
# isf is script's child, in its own session: stop it like stop() does
PID=$(pgrep -P "$PID")
stop
cmp -s "$L/big" "$R/big" || fail "big differs"
