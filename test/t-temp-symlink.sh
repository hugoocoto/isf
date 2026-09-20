#!/usr/bin/env bash
# A symlink left where isf's temp file goes isn't written through: isf makes
# its temp files, never opens ones that are there already
. "$(dirname "$0")/lib.sh"

mkdir -p "$R" "$T/outside"
echo untouched >"$T/outside/here"
echo untouched >"$T/outside/there"
start
pid=$(pgrep -g "$PID" -x isf | head -1)
[ -n "$pid" ] || fail "no isf process"
# The names it would use next, on both sides, pointing out of the folders
for i in 0 1 2 3; do ln -s "$T/outside/here" "$L/.isf.$pid.$i.tmp"; done
for i in 0 1; do ln -s "$T/outside/there" "$R/.isf.$pid.$i.tmp"; done

echo down >"$R/d.txt"
echo up >"$L/u.txt"
WAIT=20 wait_for '[ -e "$L/d.txt" ] && [ -e "$R/u.txt" ]'
settle
expect_file "$L/d.txt" down
expect_file "$R/u.txt" up
expect_file "$T/outside/here" untouched
expect_file "$T/outside/there" untouched
