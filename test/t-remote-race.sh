#!/usr/bin/env bash
# The remote changes while a file is sent over it: the upload stops short of
# replacing it, and the two versions are then handled as a conflict, so the
# remote edit isn't lost
. "$(dirname "$0")/lib.sh"

export ISF_TEST_LATENCY_MS=100 # the upload takes a few seconds
WAIT=40
echo v1 >"$L/big"
start
wait_same

yes local | head -c $((16 << 20)) >"$L/big"
sleep 1 # the upload is under way
echo "remote edit" >"$R/big"
wait_same

expect_file "$R/big" "remote edit"
[ "$(head -c 5 "$R/big.isf-conflict")" = local ] || fail "the local version wasn't kept"
