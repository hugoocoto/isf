#!/usr/bin/env bash
# Several folders at once: each keeps to itself, on both sides
. "$(dirname "$0")/lib.sh"

mkdir -p "$T/local/one/deep" "$T/local/two" "$T/home/backups"
echo one >"$T/local/one/a"
echo one-deep >"$T/local/one/deep/d"
echo two >"$T/local/two/a"
printf '*.skip\n' >"$T/local/one/.isfignore"
echo x >"$T/local/one/dropped.skip"
echo x >"$T/local/two/kept.skip"

(cd "$T/local" && exec setsid "$ISF" one two host:backups/ -I "$ISF" >"$T/out" 2>"$T/err") &
PID=$!
WAIT=20 wait_for 'grep -q "watching for changes" "$T/out"'
WAIT=20 wait_for '[ -e "$T/home/backups/one/a" ] && [ -e "$T/home/backups/two/a" ]'
settle
expect_file "$T/home/backups/one/deep/d" one-deep
expect_missing "$T/home/backups/one/dropped.skip"
[ -e "$T/home/backups/two/kept.skip" ] || fail "the other folder's file was ignored too"

# Changes there, in both folders, including a rename
echo there1 >"$T/home/backups/one/from-there"
echo there2 >"$T/home/backups/two/from-there"
mv "$T/home/backups/one/a" "$T/home/backups/one/renamed"
WAIT=20 wait_for '[ -e "$T/local/one/from-there" ] && [ -e "$T/local/two/from-there" ] && [ -e "$T/local/one/renamed" ]'
settle
expect_file "$T/local/one/from-there" there1
expect_file "$T/local/two/from-there" there2
expect_missing "$T/local/one/a"
[ -e "$T/local/two/a" ] || fail "the other folder's file went with it"
expect_out "one/"

# And in both, the other way
echo here1 >"$T/local/one/here"
echo here2 >"$T/local/two/here"
WAIT=20 wait_for '[ -e "$T/home/backups/one/here" ] && [ -e "$T/home/backups/two/here" ]'
settle
[ "$(cat "$T/local/one/here")" = here1 ] && [ "$(cat "$T/home/backups/two/here")" = here2 ] || fail "they got mixed up"
