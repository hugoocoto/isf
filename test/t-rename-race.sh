#!/usr/bin/env bash
# Renames that cross: the same file renamed on both sides, a folder renamed
# there while files go into it here, and a rename onto a name in use
. "$(dirname "$0")/lib.sh"

mkdir -p "$R"
echo content >"$L/a"
mkdir -p "$L/d"
echo inside >"$L/d/f"
start
wait_same
settle

# The same file renamed on both sides, to different names: nothing is lost
stop
mv "$L/a" "$L/here"
mv "$R/a" "$R/there"
start
WAIT=20 wait_same
settle
found=$(ls "$L" | grep -c '^here$\|^there$')
[ "$found" -ge 1 ] || fail "the file is gone: $(ls "$L")"
for n in here there; do
        if [ -e "$L/$n" ]; then expect_file "$L/$n" content; fi
done
stop

# A folder renamed there while a file is added to it here
start
echo new >"$L/d/added"
mv "$R/d" "$R/dd"
WAIT=20 wait_same
settle
grep -rq new "$L" || fail "the file added here is gone"
grep -rq inside "$L" || fail "what was in the folder is gone"
stop

# A rename onto a name that's in use on the other side
echo one >"$L/x"
echo two >"$L/y"
start
wait_same
settle
mv "$L/x" "$L/y"
WAIT=20 wait_same
settle
expect_file "$R/y" one
expect_missing "$R/x"
