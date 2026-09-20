#!/usr/bin/env bash
# --once: sync what's different now, and exit
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
echo here >"$L/a"
echo there >"$R/b"
run ./proj host:proj --once
[ "$STATUS" = 0 ] || fail "--once failed: $(cat "$T/err")"
expect_out "in sync: 1 sent, 1 received"
expect_file "$R/a" here
expect_file "$L/b" there
grep -q "watching for changes" "$T/out" && fail "it went on watching"

# Nothing to do the second time
run ./proj --once
[ "$STATUS" = 0 ] || fail "--once failed the second time"
expect_out "already in sync"

# What it can't sync makes it exit 1
mkdir -p "$L/ro"
echo x >"$L/ro/f"
run ./proj --once
chmod 500 "$R/ro"
trap 'chmod 755 "$R/ro" 2>/dev/null; cleanup' EXIT
echo y >"$L/ro/g"
run ./proj --once
[ "$STATUS" = 1 ] || fail "it didn't say something failed (exit $STATUS)"
expect_out "not all in sync"
