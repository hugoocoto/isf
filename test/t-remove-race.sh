#!/usr/bin/env bash
# A folder deleted here while a file in it is edited there: the removals check
# each file first, so the edit isn't deleted with the rest, and comes back
. "$(dirname "$0")/lib.sh"

export ISF_TEST_LATENCY_MS=1000 # the checks reach the remote 0.5 s after they're sent
count_requests
WAIT=60
mkdir -p "$L/d"
for i in $(seq 40); do echo "$i" >"$L/d/f$i"; done
start
wait_same

# Edit once the checks of the removals are sent: after the folder was listed
# (and the removals planned), before the checks get there
before=$(requests LSTAT)
rm -r "$L/d"
wait_for '[ "$(requests LSTAT)" -ge $((before + 40)) ]'
echo edited >"$R/d/f7"
wait_same
expect_file "$L/d/f7" edited
expect_missing "$R/d/f8"

# Edited once the checks came back, as the removals go: the files were moved
# aside before they were checked, so the edit makes a new one, which stays
mkdir -p "$L/e"
for i in $(seq 10); do echo "$i" >"$L/e/f$i"; done
wait_same
before=$(requests REMOVE)
rm -r "$L/e"
wait_for '[ "$(requests REMOVE)" -ge $((before + 10)) ]'
echo edited >"$R/e/f9"
wait_same
expect_file "$L/e/f9" edited
expect_missing "$R/e/f8"
if ls -a "$R/e" | grep -q '^\.isf\..*\.tmp$'; then fail "a file moved aside was left"; fi
