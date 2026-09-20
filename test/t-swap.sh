#!/usr/bin/env bash
# A file where the other side has a folder (and the other way round), and a
# conflict copy that itself differs on both sides
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
echo start >"$L/both"
start
wait_same
settle
stop

# While it's stopped: a folder here, a file there, and the other way round
rm "$L/both" && mkdir -p "$L/both/inside" && echo deep >"$L/both/inside/f"
echo file >"$R/other"
mkdir -p "$L/other/sub" && echo deeper >"$L/other/sub/g"
start
WAIT=20 wait_same
settle
grep -rq deep "$R" || fail "what was in the folder here didn't arrive"
stop

# Now the other way: a file here where a folder is there
rm -r "$L/both" && echo now-a-file >"$L/both"
start
WAIT=20 wait_same
settle
stop

# A conflict copy that differs on both sides too
echo mine >"$L/c"
echo mine >"$L/c.isf-conflict"
echo theirs >"$R/c"
echo theirs >"$R/c.isf-conflict"
start
WAIT=20 wait_same
settle
grep -rq mine "$L" || fail "what was here is gone"
grep -rq theirs "$L" || fail "what was there is gone"
stop

# .isfignore that ignores itself: it's still synced (it has to be)
printf '.isfignore\n*.skip\n' >"$L/.isfignore"
echo x >"$L/keep.txt"
echo x >"$L/drop.skip"
start
WAIT=20 wait_for '[ -e "$R/keep.txt" ]'
settle
expect_missing "$R/drop.skip"
[ -e "$R/.isfignore" ] || fail "the ignore file itself never went"
