#!/usr/bin/env bash
# A server without posix-rename: files are replaced with a plain RENAME (the
# old one removed first), and everything ends the same
. "$(dirname "$0")/lib.sh"
export ISF_TEST_SFTP_FLAGS="-P posix-rename"
count_requests

mkdir -p "$L/d"
echo a >"$L/a"
echo b >"$L/b"
echo x >"$L/d/x"
start
wait_same
echo a2 >"$L/a"
wait_same "an upload over a file"
settle
[ "$(requests READ)" = 0 ] || fail "the file sent was fetched back"
rm -r "$L/d" && echo f >"$L/d"
wait_same "a file over a directory"
mv "$L/b" "$L/c"
wait_same "a rename"
echo e >"$L/e"
wait_same
mv "$L/e" "$L/c"
wait_same "a rename over another file"
settle
! grep -q '↓' "$T/out" || fail "something sent was fetched back"
[ "$(requests READ)" = 0 ] || fail "something sent was read back"
stop

# Both changed: the older one is kept, renamed
echo local >"$L/a"
echo remote >"$R/a"
touch -d '2030-01-01 00:00:00' "$L/a"
touch -d '2029-01-01 00:00:00' "$R/a"
start
wait_same
expect_file "$R/a.isf-conflict" remote
stop

# No rename at all: a file can't be put in place, and isf says why
export ISF_TEST_SFTP_FLAGS="-P posix-rename,rename"
echo new >"$L/new"
start
expect_err "Cannot upload 'host:proj/new'"
! grep -q "No such file" "$T/err" || fail "the error is the cleanup's, not the rename's"
expect_out "not all in sync"
expect_missing "$R/new"
