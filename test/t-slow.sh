#!/usr/bin/env bash
# A slow line: what takes longer to send than ssh's keepalives allow still
# goes over, and changes made while it goes are not lost
. "$(dirname "$0")/lib.sh"

export ISF_TEST_LATENCY_MS=200 ISF_TEST_BANDWIDTH_KBPS=50
WAIT=120 RUN_TIMEOUT=120
mkdir -p "$R"
head -c 200000 /dev/urandom >"$L/big" # 4 s of line time
echo here >"$L/small"
echo there >"$R/from-there"
run ./proj host:proj --once
[ "$STATUS" = 0 ] || fail "it didn't finish: $(cat "$T/err")"
cmp -s "$L/big" "$R/big" || fail "the big one didn't arrive whole"
expect_file "$R/small" here
expect_file "$L/from-there" there

# Watching, with changes on both sides while a transfer goes on
start
head -c 200000 /dev/urandom >"$L/big2"
wait_for '[ "$(stat -c %s "$R/big2" 2>/dev/null || echo 0)" -gt 0 ] || grep -q big2 "$T/out"'
echo during >"$L/during-here"
echo during >"$R/during-there"
WAIT=120 wait_same
settle
expect_file "$R/during-here" during
expect_file "$L/during-there" during
cmp -s "$L/big2" "$R/big2" || fail "the second big one differs"
