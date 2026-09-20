#!/usr/bin/env bash
# A remote folder whose name would upset a shell: isf starts the copy there
# through one, so the name has to survive quoting
. "$(dirname "$0")/lib.sh"
WAIT=30

odd="a folder 'with' \"quotes\" \$HOME and spaces"
mkdir -p "$ISF_TEST_REMOTE/$odd"
echo there >"$ISF_TEST_REMOTE/$odd/from-there"
echo here >"$L/from-here"

(cd "$T/local" && exec setsid "$ISF" ./proj "host:$odd" -I "$ISF" >"$T/out" 2>"$T/err") &
PID=$!
wait_for 'grep -q "watching for changes" "$T/out"'
wait_for '[ -e "$L/from-there" ] && [ -e "$ISF_TEST_REMOTE/$odd/from-here" ]'
settle
expect_file "$L/from-there" there
expect_file "$ISF_TEST_REMOTE/$odd/from-here" here

# And a change on each side afterwards, so the agent there is watching it
echo more >"$ISF_TEST_REMOTE/$odd/later"
echo more >"$L/later-here"
wait_for '[ -e "$L/later" ] && [ -e "$ISF_TEST_REMOTE/$odd/later-here" ]'
settle
expect_file "$L/later" more
