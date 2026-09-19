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
