#!/usr/bin/env bash
# Records with an extra number (some builds wrote the remote ctime) still load
. "$(dirname "$0")/lib.sh"

mkdir -p "$L/d"
echo a >"$L/a"
echo b >"$L/d/b"
ln -s a "$L/l"
start
wait_same
stop

# Records with another number after STAMP, as some builds wrote them
for f in "$XDG_STATE_HOME"/isf/????????????????; do
        perl -0777 -pi -e 's/([fdl] \d+ \d+ [0-7]+ \d+):/$1 12345:/g' "$f"
        perl -0777 -ne 'exit(/[fdl] \d+ \d+ [0-7]+ \d+ 12345:/ ? 0 : 1)' "$f" || fail "the record wasn't rewritten"
done
# Deleted here meanwhile: with the record loaded, that's a deletion to send
# (without it, the remote's copy would look new and come back)
rm "$L/a"
start
wait_same
expect_missing "$R/a"
! grep -q damaged "$T/err" || fail "the record didn't load"
