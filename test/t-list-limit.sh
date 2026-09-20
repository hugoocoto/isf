#!/usr/bin/env bash
# Past the entries it lists at the start, the rest of the remote tree is
# listed over SFTP: nothing is taken for missing, and nothing is deleted
. "$(dirname "$0")/lib.sh"

CFLAGS='-O0 -DLIST_MAX=5' make -s -C "$TOP" OUT="$T/bin/isf-small" >/dev/null || fail "cannot build"
ISF=$T/bin/isf-small
count_requests

# More entries than it lists, on both sides, some of them deep
mkdir -p "$R"
for d in one two three four; do
        mkdir -p "$R/$d/inner" "$L/mine-$d"
        for i in 1 2 3; do
                echo "$d$i" >"$R/$d/f$i"
                echo "$d$i" >"$R/$d/inner/g$i"
                echo "$d$i" >"$L/mine-$d/h$i"
        done
done
echo x >"$R/top"
echo y >"$L/top-here"

start
wait_same
settle
expect_file "$L/four/inner/g3" four3
expect_file "$R/mine-four/h3" four3
[ "$(requests OPENDIR)" -gt 0 ] || fail "nothing was listed over SFTP: the limit wasn't reached"
stop

# In sync, started again: still right, nothing deleted
start
wait_same
expect_out "already in sync"
settle
expect_file "$L/four/inner/g3" four3
stop

# And a change on each side after that
echo new >"$R/four/inner/new"
echo new >"$L/mine-four/new"
start
wait_same
settle
expect_file "$L/four/inner/new" new
expect_file "$R/mine-four/new" new
