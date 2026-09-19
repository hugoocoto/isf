#!/usr/bin/env bash
# Temp files left by interrupted transfers are removed, on both sides, once
# they're old; recent ones may be in use and stay
. "$(dirname "$0")/lib.sh"

mkdir -p "$L/d" "$R/d"
old=@$(date -d '2 days ago' +%s) recent=@$(date -d '1 hour ago' +%s) # the same on both sides
for side in "$L" "$R"; do
        echo old >"$side/d/.isf.99999.0.tmp"
        touch -d "$old" "$side/d/.isf.99999.0.tmp"
        echo recent >"$side/.isf.99999.1.tmp"
        touch -d "$recent" "$side/.isf.99999.1.tmp"
done
echo keep >"$L/d/keep"
start
wait_same
expect_missing "$L/d/.isf.99999.0.tmp"
expect_missing "$R/d/.isf.99999.0.tmp"
[ -e "$L/.isf.99999.1.tmp" ] && [ -e "$R/.isf.99999.1.tmp" ] || fail "a recent one was removed"
! grep -q 'isf\.99999' "$T/out" || fail "said so: it's only for -v"
expect_file "$R/d/keep" keep
