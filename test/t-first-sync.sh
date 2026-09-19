#!/usr/bin/env bash
# The first sync: what only one side has is copied, what differs is a conflict
. "$(dirname "$0")/lib.sh"

mkdir -p "$L/dir/sub" "$L/empty" "$R/rdir/deep"
echo a >"$L/a.txt"
echo b >"$L/dir/b.txt"
echo c >"$L/dir/sub/c.txt"
printf '#!/bin/sh\n' >"$L/run.sh"
chmod 755 "$L/run.sh"
chmod 700 "$L/dir/sub"
ln -s a.txt "$L/link"
ln -s ../missing "$L/dir/dangling"
echo r >"$R/r.txt"
echo x >"$R/rdir/deep/x.txt"
chmod 600 "$R/r.txt"
head -c 300000 /dev/urandom >"$R/big.bin"

# On both sides: the same, and different (the newer one wins)
echo same >"$L/same.txt"
cp -p "$L/same.txt" "$R/same.txt"
echo old >"$L/both.txt"
echo new >"$R/both.txt"
touch -d '2020-01-01 00:00:00' "$L/both.txt"
touch -d '2021-01-01 00:00:00' "$R/both.txt"

start
wait_same
expect_out "1 conflict"
expect_file "$L/both.txt" new
expect_file "$L/both.txt.isf-conflict" old
expect_file "$R/both.txt.isf-conflict" old
[ "$(stat -c %a "$L/r.txt")" = 600 ] || fail "mode of r.txt not kept"
[ "$(readlink "$R/link")" = a.txt ] || fail "symlink not copied"
stop

# Nothing to do the second time
start
expect_out "already in sync"
