#!/usr/bin/env bash
# Changes made while a sync is going on, and a symlink that changes where it
# points
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
for i in $(seq 200); do echo "$i" >"$L/f$i"; done
export ISF_TEST_LATENCY_MS=20
start
# While the first sync goes on: more files, on both sides
for i in $(seq 20); do echo "during-$i" >"$L/during-$i"; echo "there-$i" >"$R/there-$i"; done
WAIT=40 wait_same
settle
expect_file "$R/during-20" during-20
expect_file "$L/there-20" there-20
expect_file "$R/f200" 200

# A symlink pointing somewhere else now
ln -s one "$L/link"
WAIT=20 wait_for '[ -L "$R/link" ]'
ln -sfn two "$L/link"
WAIT=20 wait_for '[ "$(readlink "$R/link")" = two ]'
ln -sfn three "$R/link"
WAIT=20 wait_for '[ "$(readlink "$L/link")" = three ]'
wait_same
settle
