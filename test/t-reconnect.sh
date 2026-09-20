#!/usr/bin/env bash
# The connection breaks: isf connects again when it can, and syncs what
# changed meanwhile on either side
. "$(dirname "$0")/lib.sh"

WAIT=30
echo a >"$L/a"
start
wait_same

# The host goes away: its sessions die, new ones are refused
touch "$T/down"
for p in $(pgrep -g "$PID"); do [ "$p" = "$PID" ] || kill -KILL "$p"; done
wait_for 'grep -q "Lost the connection" "$T/err"'
echo b >"$L/b"
echo c >"$R/c"
rm "$L/a"
sleep 2 # a few tries fail
rm "$T/down"
wait_for 'grep -q "connected to .host. again" "$T/out"'
wait_same
expect_missing "$R/a"
expect_file "$L/c" c
expect_file "$R/b" b

# Only the main SFTP session dies, while nothing happens: the next change
# finds out, and still gets there
: >"$T/err"
kill -KILL "$(pgrep -o -g "$PID" -x sftp-server)"
echo d >"$L/d"
wait_for 'grep -q "Lost the connection" "$T/err"'
wait_same
expect_file "$R/d" d

# It breaks in the middle of transfers: what was going over is sent again,
# and nothing is left half written
: >"$T/err"
head -c 3000000 /dev/urandom >"$L/big1"
head -c 3000000 /dev/urandom >"$L/big2"
head -c 3000000 /dev/urandom >"$R/big3"
wait_for 'grep -q "big" "$T/out" || [ -e "$R/big1" ] || [ -e "$L/big3" ]'
touch "$T/down"
for p in $(pgrep -g "$PID"); do [ "$p" = "$PID" ] || kill -KILL "$p"; done
wait_for 'grep -q "Lost the connection" "$T/err"'
sleep 1
rm "$T/down"
wait_for 'grep -q "connected to .host. again" "$T/out"'
WAIT=60 wait_same
cmp -s "$L/big1" "$R/big1" || fail "big1 differs"
cmp -s "$L/big3" "$R/big3" || fail "big3 differs"
for side in "$L" "$R"; do
        [ -z "$(ls -A "$side" | grep '^\.isf\..*\.tmp$')" ] || fail "a temp file was left in $side"
done
