#!/usr/bin/env bash
# Both sides changed while isf wasn't running
. "$(dirname "$0")/lib.sh"

echo base >"$L/both.txt"
echo base >"$L/edit-vs-del.txt"
echo base >"$L/del-vs-edit.txt"
mkdir -p "$L/d/sub"
echo keep >"$L/d/keep.txt"
echo gone >"$L/d/gone.txt"
echo gone >"$L/d/sub/gone.txt"
mkdir -p "$L/e"
echo keep >"$L/e/keep.txt"
echo gone >"$L/e/gone.txt"
echo base >"$L/one-side.txt"
mkdir "$L/x"
echo in >"$L/x/in"
echo base >"$L/y"
start
wait_same
stop

# Both edited: the newest wins, the other is kept as a conflict copy
echo local >"$L/both.txt"
echo remote >"$R/both.txt"
touch -d '2030-01-01 00:00:00' "$L/both.txt"
touch -d '2029-01-01 00:00:00' "$R/both.txt"
# An edit beats a deletion, both ways
echo edited >"$L/edit-vs-del.txt"
rm "$R/edit-vs-del.txt"
rm "$L/del-vs-edit.txt"
echo edited >"$R/del-vs-edit.txt"
# A directory removed on the remote, one file in it edited locally
rm -r "$R/d"
echo kept >"$L/d/keep.txt"
# ... and the other way: made again here for the edit
rm -r "$L/e"
echo "kept there" >"$R/e/keep.txt" # (another size: see the README on same-second edits)
# Only one side changed: no conflict
echo changed >"$R/one-side.txt"
# A directory replaced by a file there: the file wins
rm -r "$R/x"
echo file >"$R/x"
# A file replaced by a directory here, and changed there: the directory wins,
# the file is kept
rm "$L/y"
mkdir "$L/y"
echo z >"$L/y/z"
echo changed >"$R/y"

start
wait_same
expect_file "$L/both.txt" local
expect_file "$R/both.txt.isf-conflict" remote
expect_file "$L/both.txt.isf-conflict" remote
expect_file "$R/edit-vs-del.txt" edited
expect_file "$L/del-vs-edit.txt" edited
expect_file "$R/d/keep.txt" kept
expect_missing "$L/d/gone.txt"
expect_missing "$L/d/sub"
expect_file "$L/e/keep.txt" "kept there"
expect_missing "$R/e/gone.txt"
expect_file "$L/one-side.txt" changed
expect_missing "$L/one-side.txt.isf-conflict"
expect_file "$L/x" file
expect_file "$R/y/z" z
expect_file "$R/y.isf-conflict" changed
expect_file "$L/y.isf-conflict" changed
expect_out "1 conflict"
