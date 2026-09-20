#!/usr/bin/env bash
# Names that could confuse a protocol or a shell, and things that aren't
# files, directories or symlinks
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
cd "$L" || exit 2
printf 'x\n' >'a file with spaces'
printf 'x\n' >'quo"te'
printf 'x\n' >"apo'strophe"
printf 'x\n' >'back\slash'
printf 'x\n' >'per%scent'
printf 'x\n' >'dash-and--dashes'
printf 'x\n' >'ñandú-ünïcode'
printf 'x\n' >'new
line'
printf 'x\n' >'tab	inside'
printf 'x\n' >'.hidden'
printf 'x\n' >'name.isf-conflict'
printf 'x\n' >'not-a-temp.isf.tmpx'
mkdir -p 'dir with spaces/and
newline'
printf 'x\n' >'dir with spaces/and
newline/deep'
cd - >/dev/null || exit 2

start
wait_same
settle

# The same the other way round
cd "$R" || exit 2
printf 'y\n' >'from the other side'
printf 'y\n' >'other
side'
cd - >/dev/null || exit 2
wait_for '[ -e "$L/from the other side" ]'
wait_same
settle

# Renamed, with the odd names on both sides
mv "$L/new
line" "$L/renamed
line"
mv "$R/other
side" "$R/other renamed"
wait_same
settle
[ -e "$L/other renamed" ] || fail "the rename there didn't arrive"
[ -e "$R/renamed
line" ] || fail "the rename here didn't arrive"

# Something that isn't a file, directory or symlink: skipped, said once, and
# everything else still syncs
mkfifo "$L/a-fifo"
printf 'z\n' >"$L/after-the-fifo"
wait_for '[ -e "$R/after-the-fifo" ]'
settle
expect_missing "$R/a-fifo"
expect_err "not a file, directory or symlink"
