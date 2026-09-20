#!/usr/bin/env bash
# One folder with thousands of entries: listed there in several messages,
# read here in several replies
. "$(dirname "$0")/lib.sh"
WAIT=60

mkdir -p "$R/many" "$L/many"
for i in $(seq 2000); do echo "$i" >"$R/many/there-$i"; done
for i in $(seq 500); do echo "$i" >"$L/many/here-$i"; done
start
wait_same
settle
expect_file "$L/many/there-2000" 2000
expect_file "$R/many/here-500" 500
stop

# Started again, in sync: nothing to do
count_requests
start
expect_out "already in sync"
wait_same
stop
[ "$(requests OPENDIR)" = 0 ] || fail "it listed the folder over SFTP: $(requests OPENDIR)"

# A change in it, on each side
echo new >"$R/many/from-there"
echo new >"$L/many/from-here"
start
wait_same
settle
expect_file "$L/many/from-there" new
expect_file "$R/many/from-here" new
