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

# A record file cut short, or full of junk: isf says so and syncs anyway,
# without deleting anything
stop
echo c >"$L/c"
start
wait_same
stop
for f in "$XDG_STATE_HOME"/isf/????????????????; do
        head -c 40 "$f" >"$f.cut" && mv "$f.cut" "$f"
done
start
wait_same
settle
expect_file "$R/c" c
expect_file "$R/d/b" b
stop

for f in "$XDG_STATE_HOME"/isf/????????????????; do
        printf 'not a record at all\n\x01\x02\x03 %s\n' "junk" >"$f"
done
start
wait_same
settle
expect_file "$R/c" c
[ -e "$L/d/b" ] || fail "it deleted what was synced before"
