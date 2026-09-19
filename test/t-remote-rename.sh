#!/usr/bin/env bash
# Renames on the remote are renamed here too, not removed and fetched again
. "$(dirname "$0")/lib.sh"
count_requests

mkdir -p "$R/d/sub"
for i in $(seq 300); do echo "$i" >"$R/d/f$i"; done
echo deep >"$R/d/sub/x"
echo one >"$R/f"
echo race >"$R/r"
echo ign >"$R/k"
printf '*.bak\n' >"$R/.isfignore"
start
wait_same
requests_clear

# A folder and a file renamed there: renamed here, nothing fetched
mv "$R/d" "$R/e"
wait_same "a folder renamed there"
expect_out "↓ d → e"
mv "$R/f" "$R/g"
wait_same "a file renamed there"
expect_out "↓ f → g"
settle
[ "$(requests READ)" = 0 ] || fail "what was renamed there was fetched again: $(requests READ) reads"

# Renamed, then edited there: the edit arrives too
mv "$R/g" "$R/h" && echo edited >"$R/h"
wait_same "renamed then edited"
expect_file "$L/h" edited

# Renamed there while edited here: nothing is lost
echo "edited here" >>"$L/r"
mv "$R/r" "$R/s"
wait_same "renamed there, edited here"
grep -rqx "edited here" "$L" || fail "the edit here was lost"

# Renamed to a name that's ignored: gone here, like a removal
mv "$R/k" "$R/k.bak"
wait_for '[ ! -e "$L/k" ]'
expect_missing "$L/k.bak"
rm "$R/k.bak" # (ignored: it would stay there alone)

# Moved out of the folder there: removed here
mv "$R/e/sub" "$T/away"
wait_same "moved out there"
expect_missing "$L/e/sub"
