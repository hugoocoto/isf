#!/usr/bin/env bash
# A file written to and kept open (a log) is synced once the writes stop for a
# while, on either side, without waiting for it to be closed
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
start
exec 3>"$L/here.log" 4>"$R/there.log"
echo one >&3
echo one >&4
sleep 0.5
echo two >&3
echo two >&4
wait_for '[ "$(cat "$R/here.log" 2>/dev/null)" = "$(printf "one\ntwo")" ]'
wait_for '[ "$(cat "$L/there.log" 2>/dev/null)" = "$(printf "one\ntwo")" ]'

# And again, still open
echo three >&3
echo three >&4
wait_for 'grep -q three "$R/here.log"'
wait_for 'grep -q three "$L/there.log"'
exec 3>&- 4>&-
wait_same
