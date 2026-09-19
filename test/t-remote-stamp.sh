#!/usr/bin/env bash
# An edit on the remote that keeps the size and the mtime's second is seen:
# the agent reports it as a write, whatever size and mtime say
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
# Synced when isf starts (not from a report), then edited there the same way
printf 'ssss' >"$R/s"
touch -d @1700000000 "$R/s"
start
wait_same
printf 'tttt' >"$R/s"
touch -d @1700000000 "$R/s"
wait_for '[ "$(cat "$L/s")" = tttt ]'

# Received, then edited there again, the same size, the same mtime
printf 'aaaa' >"$R/f"
touch -d @1700000000 "$R/f"
wait_for '[ "$(cat "$L/f" 2>/dev/null)" = aaaa ]'
printf 'bbbb' >"$R/f"
touch -d @1700000000 "$R/f"
wait_for '[ "$(cat "$L/f")" = bbbb ]'

# Sent, then edited there the same way
printf 'cccc' >"$L/g"
touch -d @1700000000 "$L/g"
wait_for '[ "$(cat "$R/g" 2>/dev/null)" = cccc ]'
settle # the agent's report of isf's own write tells its ctime
printf 'dddd' >"$R/g"
touch -d @1700000000 "$R/g"
wait_for '[ "$(cat "$L/g")" = dddd ]'
wait_same
