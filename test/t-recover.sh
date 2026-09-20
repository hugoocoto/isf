#!/usr/bin/env bash
# The agent on the remote dying, and the remote folder going away
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
echo one >"$L/a"
start
wait_same
settle

# Killed, as a dropped connection would: isf starts it again and goes on
for p in $(pgrep -g "$PID"); do
        grep -qa -- '--agent' "/proc/$p/cmdline" 2>/dev/null && kill -9 "$p"
done
echo two >"$R/b"      # made while there's no agent
echo three >"$L/c"
WAIT=40 wait_for '[ -e "$L/b" ] && [ -e "$R/c" ]'
wait_same
settle
expect_file "$L/b" two
expect_err "connecting again"
expect_out "connected to 'host' again"

# Changes after that are still seen, on both sides
echo four >"$R/d"
echo five >"$L/e"
WAIT=20 wait_same
settle
stop

# The remote folder removed while isf runs: it stops instead of deleting here
start
rm -r "$R"
WAIT=30 wait_for '! kill -0 "$PID" 2>/dev/null'
wait "$PID" 2>/dev/null
PID=
[ -e "$L/a" ] || fail "it deleted what's here"
grep -q "stopping\|is empty\|was removed" "$T/err" || fail "it didn't say why it stopped: $(cat "$T/err")"
