#!/usr/bin/env bash
# A symlink out of the synced folder isn't a way into what's outside it: when
# the other side turns it into a directory, the symlink is replaced here, not
# written through
. "$(dirname "$0")/lib.sh"

mkdir -p "$R" "$T/outside"
echo untouched >"$T/outside/secret"
ln -s "$T/outside" "$R/s"
start
wait_for '[ -L "$L/s" ]'
settle
[ "$(readlink "$L/s")" = "$T/outside" ] || fail "the symlink wasn't synced"
stop

# A directory with the same name there, while isf isn't running
rm "$R/s"
mkdir "$R/s"
echo new >"$R/s/secret"
start
WAIT=20 wait_for '[ -e "$L/s/secret" ]'
settle
[ -d "$L/s" ] && [ ! -L "$L/s" ] || fail "the symlink is still there"
expect_file "$T/outside/secret" untouched
expect_missing "$T/outside/new"

# And the same while it runs, with the symlink made here
ln -s "$T/outside" "$L/t"
WAIT=20 wait_for '[ -L "$R/t" ]'
settle
rm "$R/t"
mkdir "$R/t"
echo new >"$R/t/secret"
echo new >"$R/t/new"
WAIT=20 wait_for '[ -e "$L/t/new" ]'
settle
[ -d "$L/t" ] && [ ! -L "$L/t" ] || fail "the symlink is still there"
expect_file "$T/outside/secret" untouched
expect_missing "$T/outside/new"
expect_file "$L/t/secret" new
